/*
 * PL/Proxy - easy access to partitioned database.
 *
 * Copyright (c) 2006 Sven Suursoho, Skype Technologies OÜ
 * Copyright (c) 2007 Marko Kreen, Skype Technologies OÜ
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Actual execution logic is here.
 *
 * - Tag particural databases, where query must be sent.
 * - Send the query.
 * - Fetch the results.
 */

#include "plproxy.h"

#include <sys/time.h>

#include "poll_compat.h"

#if PG_VERSION_NUM < 80400
static int geterrcode(void)
{
	/* switch context to work around Assert() in CopyErrorData() */
	MemoryContext ctx = MemoryContextSwitchTo(TopMemoryContext);
	ErrorData *edata = CopyErrorData();
	int code = edata->sqlerrcode;
	FreeErrorData(edata);
	MemoryContextSwitchTo(ctx);
	return code;
}
#endif

/* some error happened */
static void
conn_error(ProxyFunction *func, ProxyConnection *conn, const char *desc)
{
	plproxy_error(func, "%s: %s",
				  desc, PQerrorMessage(conn->db));
}

/* Compare if major/minor match. Works on "MAJ.MIN.*" */
static bool
cmp_branch(const char *this, const char *that)
{
	int dot = 0;
	int i;

	for (i = 0; this[i] || that[i]; i++)
	{
		/* allow just maj.min verson */
		if (dot && this[i] == '.' && !that[i])
			return true;
		if (dot && that[i] == '.' && !this[i])
			return true;

		/* compare, different length is also handled here */
		if (this[i] != that[i])
			return false;

		/* stop on second dot */
		if (this[i] == '.' && dot++)
			return true;
	}
	return true;
}

static void
flush_connection(ProxyFunction *func, ProxyConnection *conn)
{
	int res;

	/* flush it down */
	res = PQflush(conn->db);

	/* set actual state */
	if (res > 0)
		conn->state = C_QUERY_WRITE;
	else if (res == 0)
		conn->state = C_QUERY_READ;
	else
		conn_error(func, conn, "PQflush");
}

/*
 * Small sanity checking for new connections.
 *
 * Current checks:
 * - Does there happen any encoding conversations?
 * - Difference in standard_conforming_strings.
 */
static int
tune_connection(ProxyFunction *func, ProxyConnection *conn)
{
	const char *this_enc, *dst_enc;
	const char *dst_ver;
	StringInfo	sql = NULL;

	/*
	 * check if target server has same backend version.
	 */
	dst_ver = PQparameterStatus(conn->db, "server_version");
	conn->same_ver = cmp_branch(dst_ver, PG_VERSION);

	/*
	 * Make sure remote I/O is done using local server_encoding.
	 */
	this_enc = GetDatabaseEncodingName();
	dst_enc = PQparameterStatus(conn->db, "client_encoding");
	if (dst_enc && strcmp(this_enc, dst_enc))
	{
		if (!sql)
			sql = makeStringInfo();
		appendStringInfo(sql, "set client_encoding = '%s'; ", this_enc);
	}

	/*
	 * if second time in this function, they should be active already.
	 */
	if (sql && conn->tuning)
	{
		/* display SET query */
		appendStringInfo(sql, "-- does not seem to apply");
		conn_error(func, conn, sql->data);
	}

	/*
	 * send tuning query
	 */
	if (sql)
	{
		conn->tuning = 1;
		conn->state = C_QUERY_WRITE;
		if (!PQsendQuery(conn->db, sql->data))
			conn_error(func, conn, "PQsendQuery");
		pfree(sql->data);
		pfree(sql);

		flush_connection(func, conn);
		return 1;
	}

	conn->tuning = 0;
	return 0;
}

/* send the query to server connection */
static void
send_query(ProxyFunction *func, ProxyConnection *conn,
		   const char **values, int *plengths, int *pformats)
{
	int			res;
	struct timeval now;
	ProxyQuery *q = func->remote_sql;
	ProxyConfig *cf = &func->cur_cluster->config;
	int			binary_result = 0;

	gettimeofday(&now, NULL);
	conn->query_time = now.tv_sec;

	tune_connection(func, conn);
	if (conn->tuning)
		return;

	/* use binary result only on same backend ver */
	if (cf->disable_binary == 0 && conn->same_ver)
	{
		/* binary recv for non-record types */
		if (func->ret_scalar)
		{
			if (func->ret_scalar->has_recv)
				binary_result = 1;
		}
		else
		{
			if (func->ret_composite->use_binary)
				binary_result = 1;
		}
	}

	/* send query */
	conn->state = C_QUERY_WRITE;
	res = PQsendQueryParams(conn->db, q->sql, q->arg_count,
							NULL,		/* paramTypes */
							values,		/* paramValues */
							plengths,	/* paramLengths */
							pformats,	/* paramFormats */
							binary_result);		/* resultformat, 0-text, 1-bin */
	if (!res)
		conn_error(func, conn, "PQsendQueryParams");

	/* flush it down */
	flush_connection(func, conn);
}

/* returns false of conn should be dropped */
static bool
check_old_conn(ProxyFunction *func, ProxyConnection *conn, struct timeval * now)
{
	time_t		t;
	int			res;
	struct pollfd	pfd;
	ProxyConfig *cf = &func->cur_cluster->config;

	if (PQstatus(conn->db) != CONNECTION_OK)
		return false;

	/* check if too old */
	if (cf->connection_lifetime > 0)
	{
		t = now->tv_sec - conn->connect_time;
		if (t >= cf->connection_lifetime)
			return false;
	}

	/* how long ts been idle */
	t = now->tv_sec - conn->query_time;
	if (t < PLPROXY_IDLE_CONN_CHECK)
		return true;

	/*
	 * Simple way to check if old connection is stable - look if there
	 * are events pending.  If there are drop the connection.
	 */
intr_loop:
	pfd.fd = PQsocket(conn->db);
	pfd.events = POLLIN;
	pfd.revents = 0;

	res = poll(&pfd, 1, 0);
	if (res > 0)
	{
		elog(WARNING, "PL/Proxy: detected unstable connection");
		return false;
	}
	else if (res < 0)
	{
		if (errno == EINTR)
			goto intr_loop;
		plproxy_error(func, "check_old_conn: select failed: %s",
					  strerror(errno));
	}

	/* seems ok */
	return true;
}

static void
handle_notice(void *arg, const PGresult *res)
{
	ProxyCluster *cluster = arg;
	plproxy_remote_error(cluster->cur_func, res, false);
}

/* check existing conn status or launch new conn */
static void
prepare_conn(ProxyFunction *func, ProxyConnection *conn)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	/* state should be C_READY or C_NONE */
	switch (conn->state)
	{
		case C_DONE:
			conn->state = C_READY;
		case C_READY:
			if (check_old_conn(func, conn, &now))
				return;

		case C_CONNECT_READ:
		case C_CONNECT_WRITE:
		case C_QUERY_READ:
		case C_QUERY_WRITE:
			/* close rotten connection */
			elog(NOTICE, "PL/Proxy: dropping stale conn");
			PQfinish(conn->db);
			conn->db = NULL;
			conn->state = C_NONE;
			conn->tuning = 0;
		case C_NONE:
			break;
	}

	conn->connect_time = now.tv_sec;

	/* launch new connection */
	conn->db = PQconnectStart(conn->connstr);
	if (conn->db == NULL)
		plproxy_error(func, "No memory for PGconn");

	/* tag connection dirty */
	conn->state = C_CONNECT_WRITE;

	if (PQstatus(conn->db) == CONNECTION_BAD)
		conn_error(func, conn, "PQconnectStart");

	/* override default notice handler */
	PQsetNoticeReceiver(conn->db, handle_notice, func->cur_cluster);
}

/*
 * Connection has a resultset avalable, fetch it.
 *
 * Returns true if there may be more results coming,
 * false if all done.
 */
static bool
another_result(ProxyFunction *func, ProxyConnection *conn)
{
	PGresult   *res;

	/* got one */
	res = PQgetResult(conn->db);
	if (res == NULL)
	{
		if (conn->tuning)
			conn->state = C_READY;
		else
			conn->state = C_DONE;
		return false;
	}

	switch (PQresultStatus(res))
	{
		case PGRES_TUPLES_OK:
			if (conn->res)
			{
				PQclear(res);
				conn_error(func, conn, "double result?");
			}
			conn->res = res;
			break;
		case PGRES_COMMAND_OK:
			PQclear(res);
			break;
		case PGRES_FATAL_ERROR:
			if (conn->res)
				PQclear(conn->res);
			conn->res = res;

			plproxy_remote_error(func, res, true);
			break;
		default:
			if (conn->res)
				PQclear(conn->res);
			conn->res = res;

			plproxy_error(func, "Unexpected result type: %s", PQresStatus(PQresultStatus(res)));
			break;
	}
	return true;
}

/*
 * Called when select() told that conn is avail for reading/writing.
 *
 * It should call postgres handlers and then change state if needed.
 */
static void
handle_conn(ProxyFunction *func, ProxyConnection *conn)
{
	int			res;
	PostgresPollingStatusType poll_res;

	switch (conn->state)
	{
		case C_CONNECT_READ:
		case C_CONNECT_WRITE:
			poll_res = PQconnectPoll(conn->db);
			switch (poll_res)
			{
				case PGRES_POLLING_WRITING:
					conn->state = C_CONNECT_WRITE;
					break;
				case PGRES_POLLING_READING:
					conn->state = C_CONNECT_READ;
					break;
				case PGRES_POLLING_OK:
					conn->state = C_READY;
					break;
				case PGRES_POLLING_ACTIVE:
				case PGRES_POLLING_FAILED:
					conn_error(func, conn, "PQconnectPoll");
			}
			break;
		case C_QUERY_WRITE:
			flush_connection(func, conn);
			break;
		case C_QUERY_READ:
			res = PQconsumeInput(conn->db);
			if (res == 0)
				conn_error(func, conn, "PQconsumeInput");

			/* loop until PQgetResult returns NULL */
			while (1)
			{
				/* if PQisBusy, then incomplete result */
				if (PQisBusy(conn->db))
					break;

				/* got one */
				if (!another_result(func, conn))
					break;
			}
		case C_NONE:
		case C_DONE:
		case C_READY:
			break;
	}
}

/*
 * Check if tagged connections have interesting events.
 *
 * Currenly uses select() as it should be enough
 * on small number of sockets.
 */
static int
poll_conns(ProxyFunction *func, ProxyCluster *cluster)
{
	static struct pollfd *pfd_cache = NULL;
	static int pfd_allocated = 0;

	int			i,
				res,
				fd;
	ProxyConnection *conn;
	struct pollfd *pf;
	int numfds = 0;
	int ev = 0;

	if (pfd_allocated < cluster->conn_count)
	{
		struct pollfd *tmp;
		int num = cluster->conn_count;
		if (num < 64)
			num = 64;
		if (pfd_cache == NULL)
			tmp = malloc(num * sizeof(struct pollfd));
		else
			tmp = realloc(pfd_cache, num * sizeof(struct pollfd));
		if (!tmp)
			elog(ERROR, "no mem for pollfd cache");
		pfd_cache = tmp;
		pfd_allocated = num;
	}

	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];
		if (!conn->run_tag)
			continue;

		/* decide what to do */
		switch (conn->state)
		{
			case C_DONE:
			case C_READY:
			case C_NONE:
				continue;
			case C_CONNECT_READ:
			case C_QUERY_READ:
				ev = POLLIN;
				break;
			case C_CONNECT_WRITE:
			case C_QUERY_WRITE:
				ev = POLLOUT;
				break;
		}

		/* add fd to proper set */
		pf = pfd_cache + numfds++;
		pf->fd = PQsocket(conn->db);
		pf->events = ev;
		pf->revents = 0;
	}

	/* wait for events */
	res = poll(pfd_cache, numfds, 1000);
	if (res == 0)
		return 0;
	if (res < 0)
	{
		if (errno == EINTR)
			return 0;
		plproxy_error(func, "poll() failed: %s", strerror(errno));
	}

	/* now recheck the conns */
	pf = pfd_cache;
	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];
		if (!conn->run_tag)
			continue;

		switch (conn->state)
		{
			case C_DONE:
			case C_READY:
			case C_NONE:
				continue;
			case C_CONNECT_READ:
			case C_QUERY_READ:
			case C_CONNECT_WRITE:
			case C_QUERY_WRITE:
				break;
		}

		/*
		 * they should be in same order as called,
		 */
		fd = PQsocket(conn->db);
		if (pf->fd != fd)
			elog(WARNING, "fd order from poll() is messed up?");

		if (pf->revents)
			handle_conn(func, conn);

		pf++;
	}
	return 1;
}

/* Check if some operation has gone over limit */
static void
check_timeouts(ProxyFunction *func, ProxyCluster *cluster, ProxyConnection *conn, time_t now)
{
	ProxyConfig *cf = &cluster->config;

	switch (conn->state)
	{
		case C_CONNECT_READ:
		case C_CONNECT_WRITE:
			if (cf->connect_timeout <= 0)
				break;
			if (now - conn->connect_time <= cf->connect_timeout)
				break;
			plproxy_error(func, "connect timeout to: %s", conn->connstr);
			break;

		case C_QUERY_READ:
		case C_QUERY_WRITE:
			if (cf->query_timeout <= 0)
				break;
			if (now - conn->query_time <= cf->query_timeout)
				break;
			plproxy_error(func, "query timeout");
			break;
		default:
			break;
	}
}

/* Run the query on all tagged connections in parallel */
static void
remote_execute(ProxyFunction *func)
{
	ExecStatusType err;
	ProxyConnection *conn;
	ProxyCluster *cluster = func->cur_cluster;
	int			i,
				pending = 0;
	struct timeval now;

	/* either launch connection or send query */
	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];
		if (!conn->run_tag)
			continue;

		/* check if conn is alive, and launch if not */
		prepare_conn(func, conn);
		pending++;

		/* if conn is ready, then send query away */
		if (conn->state == C_READY)
			send_query(func, conn, conn->param_values, conn->param_lengths, conn->param_formats);
	}

	/* now loop until all results are arrived */
	while (pending)
	{
		/* allow postgres to cancel processing */
		CHECK_FOR_INTERRUPTS();

		/* wait for events */
		if (poll_conns(func, cluster) == 0)
			continue;

		/* recheck */
		pending = 0;
		gettimeofday(&now, NULL);
		for (i = 0; i < cluster->conn_count; i++)
		{
			conn = &cluster->conn_list[i];
			if (!conn->run_tag)
				continue;

			/* login finished, send query */
			if (conn->state == C_READY)
				send_query(func, conn, conn->param_values, conn->param_lengths, conn->param_formats);

			if (conn->state != C_DONE)
				pending++;

			check_timeouts(func, cluster, conn, now.tv_sec);
		}
	}

	/* review results, calculate total */
	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];

		if ((conn->run_tag || conn->res)
			&& !(conn->run_tag && conn->res))
			plproxy_error(func, "run_tag does not match res");

		if (!conn->run_tag)
			continue;

		if (conn->state != C_DONE)
			plproxy_error(func, "Unfinished connection");
		if (conn->res == NULL)
			plproxy_error(func, "Lost result");

		err = PQresultStatus(conn->res);
		if (err != PGRES_TUPLES_OK)
			plproxy_error(func, "Remote error: %s",
						  PQresultErrorMessage(conn->res));

		cluster->ret_total += PQntuples(conn->res);
	}
}

static void
remote_cancel(ProxyFunction *func)
{
	ProxyConnection *conn;
	ProxyCluster *cluster = func->cur_cluster;
	PGcancel *cancel;
	char errbuf[256];
	int ret;
	int i;

	if (cluster == NULL)
		return;

	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];
		if (conn->state == C_NONE ||
			conn->state == C_READY ||
			conn->state == C_DONE)
			continue;

		cancel = PQgetCancel(conn->db);
		if (cancel == NULL)
		{
			elog(NOTICE, "Invalid connection!");
			continue;
		}
		ret = PQcancel(cancel, errbuf, sizeof(errbuf));
		PQfreeCancel(cancel);
		if (ret == 0)
			elog(NOTICE, "Cancel query failed!");
	}
}

/*
 * Run hash function and tag connections. If any of the hash function 
 * arguments are mentioned in the split_arrays an element of the array
 * is used instead of the actual array.
 */
static void
tag_hash_partitions(ProxyFunction *func, FunctionCallInfo fcinfo, int tag,
					DatumArray **array_params, int array_row)
{
	int			i;
	TupleDesc	desc;
	Oid			htype;
	ProxyCluster *cluster = func->cur_cluster;

	/* execute cached plan */
	plproxy_query_exec(func, fcinfo, func->hash_sql, array_params, array_row);

	/* get header */
	desc = SPI_tuptable->tupdesc;
	htype = SPI_gettypeid(desc, 1);

	/* tag connections */
	for (i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		uint32		hashval = 0;
		HeapTuple	row = SPI_tuptable->vals[i];
		Datum		val = SPI_getbinval(row, desc, 1, &isnull);

		if (isnull)
			plproxy_error(func, "Hash function returned NULL");

		if (htype == INT4OID)
			hashval = DatumGetInt32(val);
		else if (htype == INT8OID)
			hashval = DatumGetInt64(val);
		else if (htype == INT2OID)
			hashval = DatumGetInt16(val);
		else
			plproxy_error(func, "Hash result must be int2, int4 or int8");

		hashval &= cluster->part_mask;
		cluster->part_map[hashval]->run_tag = tag;
	}

	/* sanity check */
	if (SPI_processed == 0 || SPI_processed > 1)
		if (!fcinfo->flinfo->fn_retset)
			plproxy_error(func, "Only set-returning function"
						  " allows hashcount <> 1");
}

/*
 * Deconstruct an array type to array of Datums, note NULL elements
 * and determine the element type information.
 */
static DatumArray *
make_datum_array(ProxyFunction *func, ArrayType *v, Oid elem_type)
{
	DatumArray	   *da = palloc0(sizeof(*da));

	da->type = plproxy_find_type_info(func, elem_type, true);

	if (v)
		deconstruct_array(v,
						  da->type->type_oid, da->type->length, da->type->by_value,
						  da->type->alignment,
						  &da->values, &da->nulls, &da->elem_count);
	return da;
}

/*
 * Evaluate the run condition. Tag the matching connections with the specified
 * tag.
 *
 * Note that we don't allow nested plproxy calls on the same cluster (ie.
 * remote hash functions). The cluster and connection state are global and
 * would easily get messed up.
 */
static void
tag_run_on_partitions(ProxyFunction *func, FunctionCallInfo fcinfo, int tag,
					  DatumArray **array_params, int array_row)
{
	ProxyCluster   *cluster = func->cur_cluster;
	int				i;

	switch (func->run_type)
	{
		case R_HASH:
			tag_hash_partitions(func, fcinfo, tag, array_params, array_row);
			break;
		case R_ALL:
			for (i = 0; i < cluster->part_count; i++)
				cluster->part_map[i]->run_tag = tag;
			break;
		case R_EXACT:
			i = func->exact_nr;
			if (i < 0 || i >= cluster->part_count)
				plproxy_error(func, "part number out of range");
			cluster->part_map[i]->run_tag = tag;
			break;
		case R_ANY:
			i = random() & cluster->part_mask;
			cluster->part_map[i]->run_tag = tag;
			break;
		default:
			plproxy_error(func, "uninitialized run_type");
	}
}

static int get_int(ProxyFunction *func, TupleDesc desc, HeapTuple row, int col, Oid oid)
{
	bool isnull;
	Datum val = SPI_getbinval(row, desc, col, &isnull);
	if (isnull)
		plproxy_error(func, "expected not-NULL value");
	if (oid == INT4OID)
		return DatumGetInt32(val);
	else if (oid == INT8OID)
		return DatumGetInt64(val);
	else if (oid == INT2OID)
		return DatumGetInt16(val);
	plproxy_error(func, "expected int arg");
	return 0;
}

/*
 * Add all values on row to per-connection arrays
 */
static void split_value(ProxyFunction *func, DatumArray *arrays_to_split[], ProxyConnection *conn, int row)
{
	int col;

	if (!conn->bstate)
		conn->bstate = palloc0(func->arg_count * sizeof(*conn->bstate));

	/* Add this set of elements to the partition specific arrays */
	for (col = 0; col < func->arg_count; col++)
	{
		if (!IS_SPLIT_ARG(func, col))
			continue;

		conn->bstate[col] = accumArrayResult(conn->bstate[col],
											 arrays_to_split[col]->values[row],
											 arrays_to_split[col]->nulls[row],
											 arrays_to_split[col]->type->type_oid,
											 CurrentMemoryContext);
	}
}

/*
 * Calculate all hashes with single query
 */
static void new_split_args(ProxyFunction *func, FunctionCallInfo fcinfo,
						   int split_array_count,
						   int split_array_len,
						   DatumArray *arrays_to_split[])
{
	int			i;
	TupleDesc	desc;
	Oid			oid1, oid2;
	ProxyCluster *cluster = func->cur_cluster;

	/* execute cached plan */
	plproxy_query_exec(func, fcinfo, func->hash_sql, NULL, 0);

	/* get header */
	desc = SPI_tuptable->tupdesc;
	oid1 = SPI_gettypeid(desc, 1);
	oid2 = SPI_gettypeid(desc, 2);

	/* split arrays */
	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple	row = SPI_tuptable->vals[i];
		uint32		idx = get_int(func, desc, row, 1, oid1);
		uint32		hash = get_int(func, desc, row, 2, oid2);
		ProxyConnection *conn;

		hash &= cluster->part_mask;
		conn = cluster->part_map[hash];
		if (conn->run_tag == idx)
			continue;
		conn->run_tag = idx;

		split_value(func, arrays_to_split, conn, idx - 1);
	}
}

/*
 * Loop over arrays, handle each row separately.
 */
static void old_split_args(ProxyFunction *func, FunctionCallInfo fcinfo,
						   int split_array_count,
						   int split_array_len,
						   DatumArray *arrays_to_split[])
{
	int					row;
	ProxyCluster	   *cluster = func->cur_cluster;

	/* Need to split, evaluate the RUN ON condition for each of the elements. */
	for (row = 0; row < split_array_len; row++)
	{
		int		part;
		int		my_tag = row+1;

		/*
		 * Tag the run-on partitions with a tag that allows us us to identify
		 * which partitions need the set of elements from this row.
		 */
		tag_run_on_partitions(func, fcinfo, my_tag, arrays_to_split, row);

		/* Add the array elements to the partitions tagged in previous step */
		for (part = 0; part < cluster->conn_count; part++)
		{
			ProxyConnection	   *conn = &cluster->conn_list[part];

			if (conn->run_tag != my_tag)
				continue;

			split_value(func, arrays_to_split, conn, row);
		}
	}
}

/*
 * Tag the partitions to be run on, if split is requested prepare the 
 * per-partition split array parameters.
 *
 * This is done by looping over all of the split arrays side-by-side, for each
 * tuple see if it satisfies the RUN ON condition. If so, copy the tuple
 * to the partition's private array parameters.
 */
static void
prepare_and_tag_partitions(ProxyFunction *func, FunctionCallInfo fcinfo)
{
	int					i, col;
	int					split_array_len = -1;
	int					split_array_count = 0;
	ProxyCluster	   *cluster = func->cur_cluster;
	DatumArray		   *arrays_to_split[FUNC_MAX_ARGS];

	/*
	 * See if we have any arrays to split. If so, make them manageable by
	 * converting them to Datum arrays. During the process verify that all
	 * the arrays are of the same length.
	 */
	for (i = 0; i < func->arg_count; i++)
	{
		ArrayType	   *v;

		if (!IS_SPLIT_ARG(func, i))
		{
			arrays_to_split[i] = NULL;
			continue;
		}

		if (PG_ARGISNULL(i))
			v = NULL;
		else
		{
			v = PG_GETARG_ARRAYTYPE_P(i);

			if (ARR_NDIM(v) > 1)
				plproxy_error(func, "split multi-dimensional arrays are not supported");
		}

		arrays_to_split[i] = make_datum_array(func, v, func->arg_types[i]->elem_type);

		/* Check that the element counts match */
		if (split_array_len < 0)
			split_array_len = arrays_to_split[i]->elem_count;
		else if (arrays_to_split[i]->elem_count != split_array_len)
			plproxy_error(func, "split arrays must be of identical lengths");

		++split_array_count;
	}

	/* If nothing to split, just tag the partitions and be done with it */
	if (!split_array_count)
	{
		tag_run_on_partitions(func, fcinfo, 1, NULL, 0);
		return;
	}

	if (func->new_split)
		new_split_args(func, fcinfo, split_array_count, split_array_len, arrays_to_split);
	else
		old_split_args(func, fcinfo, split_array_count, split_array_len, arrays_to_split);

	/*
	 * Finally, copy the accumulated arrays to the actual connections
	 * to be used as parameters.
	 */
	for (i = 0; i < cluster->conn_count; i++)
	{
		ProxyConnection *conn = &cluster->conn_list[i];

		if (!conn->run_tag)
			continue;

		conn->split_params = palloc(func->arg_count * sizeof(*conn->split_params));

		for (col = 0; col < func->arg_count; col++)
		{
			if (!IS_SPLIT_ARG(func, col))
				conn->split_params[col] = PointerGetDatum(NULL);
			else
				conn->split_params[col] = makeArrayResult(conn->bstate[col],
														  CurrentMemoryContext);
		}
	}
}

/*
 * Prepare parameters for the query.
 */
static void
prepare_query_parameters(ProxyFunction *func, FunctionCallInfo fcinfo)
{
	int				i;
	ProxyCluster   *cluster = func->cur_cluster;

	for (i = 0; i < func->remote_sql->arg_count; i++)
	{
		int			idx = func->remote_sql->arg_lookup[i];
		bool		bin = cluster->config.disable_binary ? 0 : 1;
		const char *fixed_param_val = NULL;
		int			fixed_param_len, fixed_param_fmt;
		int			part;

		/* Avoid doing multiple conversions for fixed parameters */
		if (!IS_SPLIT_ARG(func, idx) && !PG_ARGISNULL(idx))
		{
			fixed_param_val = plproxy_send_type(func->arg_types[idx],
												PG_GETARG_DATUM(idx),
												bin,
												&fixed_param_len,
												&fixed_param_fmt);
		}

		/* Add the parameters to partitions */
		for (part = 0; part < cluster->conn_count; part++)
		{
			ProxyConnection *conn = &cluster->conn_list[part];

			if (!conn->run_tag)
				continue;

			if (PG_ARGISNULL(idx))
			{
				conn->param_values[i] = NULL;
				conn->param_lengths[i] = 0;
				conn->param_formats[i] = 0;
			}
			else
			{
				if (IS_SPLIT_ARG(func, idx))
				{
					conn->param_values[i] = plproxy_send_type(func->arg_types[idx],
															  conn->split_params[idx],
															  bin,
															  &conn->param_lengths[i],
															  &conn->param_formats[i]);
				}
				else
				{
					conn->param_values[i] = fixed_param_val;
					conn->param_lengths[i] = fixed_param_len;
					conn->param_formats[i] = fixed_param_fmt;
				}
			}
		}
	}
}

/* Clean old results and prepare for new one */
void
plproxy_clean_results(ProxyCluster *cluster)
{
	int					i;
	ProxyConnection	   *conn;

	if (!cluster)
		return;

	cluster->ret_total = 0;
	cluster->ret_cur_conn = 0;

	for (i = 0; i < cluster->conn_count; i++)
	{
		conn = &cluster->conn_list[i];
		if (conn->res)
		{
			PQclear(conn->res);
			conn->res = NULL;
		}
		conn->pos = 0;
		conn->run_tag = 0;
		conn->bstate = NULL;
	}
	/* conn state checks are done in prepare_conn */
}

/* Select partitions and execute query on them */
void
plproxy_exec(ProxyFunction *func, FunctionCallInfo fcinfo)
{
	/*
	 * Prepare parameters and run query.  On cancel, send cancel request to
	 * partitions too.
	 */
	PG_TRY();
	{
		func->cur_cluster->busy = true;
		func->cur_cluster->cur_func = func;

		/* clean old results */
		plproxy_clean_results(func->cur_cluster);

		/* tag the partitions and prepare per-partition parameters */
		prepare_and_tag_partitions(func, fcinfo);

		/* prepare the target query parameters */
		prepare_query_parameters(func, fcinfo);

		remote_execute(func);

		func->cur_cluster->busy = false;
	}
	PG_CATCH();
	{
		func->cur_cluster->busy = false;

		if (geterrcode() == ERRCODE_QUERY_CANCELED)
			remote_cancel(func);

		/* plproxy_remote_error() cannot clean itself, do it here */
		plproxy_clean_results(func->cur_cluster);

		PG_RE_THROW();
	}
	PG_END_TRY();
}
