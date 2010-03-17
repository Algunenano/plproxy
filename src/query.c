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
 * SQL statement generation helpers.
 */

#include "plproxy.h"

struct ArgRef {
	int end;
	int sql_idx;
};

/*
 * Temporary info structure for generation.
 *
 * Later it will be used to make ProxyQuery.
 */
struct QueryBuffer
{
	ProxyFunction *func;
	StringInfo	sql;
	int			arg_count;
	int		   *arg_lookup;
	bool		add_types;
	bool		track_refs;
	int			ref_count;
	struct ArgRef refs[FUNC_MAX_ARGS];
};

/*
 * Prepare temporary structure for query generation.
 */
QueryBuffer *
plproxy_query_start(ProxyFunction *func, bool add_types, bool track_refs)
{
	QueryBuffer *q = palloc(sizeof(*q));

	q->func = func;
	q->sql = makeStringInfo();
	q->arg_count = 0;
	q->add_types = add_types;
	q->track_refs = track_refs;
	q->ref_count = 0;
	q->arg_lookup = palloc(sizeof(int) * func->arg_count);
	return q;
}

/*
 * Add string fragment to query.
 */
bool
plproxy_query_add_const(QueryBuffer *q, const char *data)
{
	appendStringInfoString(q->sql, data);
	return true;
}

/*
 * Helper for adding a parameter reference to the query
 */
static void
add_ref(StringInfo buf, int sql_idx, ProxyFunction *func, int fn_idx, bool add_type)
{
	char		tmp[1 + 3 + 2 + NAMEDATALEN*2 + 1];

	if (add_type)
		snprintf(tmp, sizeof(tmp), "$%d::%s", sql_idx + 1,
				func->arg_types[fn_idx]->name);
	else
		snprintf(tmp, sizeof(tmp), "$%d", sql_idx + 1);
	appendStringInfoString(buf, tmp);
}

/*
 * Add a SQL identifier to the query that may possibly be
 * a parameter reference.
 */
bool
plproxy_query_add_ident(QueryBuffer *q, const char *ident)
{
	int			i,
				fn_idx = -1,
				sql_idx = -1;
	struct ArgRef *ref = NULL;

	fn_idx = plproxy_get_parameter_index(q->func, ident);

	if (fn_idx >= 0)
	{
		for (i = 0; i < q->arg_count; i++)
		{
			if (q->arg_lookup[i] == fn_idx)
			{
				sql_idx = i;
				break;
			}
		}
		if (sql_idx < 0)
		{
			sql_idx = q->arg_count++;
			q->arg_lookup[sql_idx] = fn_idx;
		}
		add_ref(q->sql, sql_idx, q->func, fn_idx, q->add_types);
		if (q->track_refs) {
			if (q->ref_count >= FUNC_MAX_ARGS)
				elog(ERROR, "too many args to hash func");
			ref = &q->refs[q->ref_count++];
			ref->end = q->sql->len;
			ref->sql_idx = sql_idx;
		}
	}
	else
	{
		if (ident[0] == '$')
			return false;
		appendStringInfoString(q->sql, ident);
	}

	return true;
}

/*
 * Create a ProxyQuery based on temporary QueryBuffer.
 */
ProxyQuery *
plproxy_query_finish(QueryBuffer *q)
{
	ProxyQuery *pq;
	MemoryContext old;
	int			len;

	old = MemoryContextSwitchTo(q->func->ctx);

	pq = palloc(sizeof(*pq));
	pq->sql = pstrdup(q->sql->data);
	pq->arg_count = q->arg_count;
	len = q->arg_count * sizeof(int);
	pq->arg_lookup = palloc(len);
	pq->plan = NULL;

	memcpy(pq->arg_lookup, q->arg_lookup, len);

	MemoryContextSwitchTo(old);

	/* unnecessary actually, but lets be correct */
	if (1)
	{
		pfree(q->sql->data);
		pfree(q->sql);
		pfree(q->arg_lookup);
		memset(q, 0, sizeof(*q));
		pfree(q);
	}

	return pq;
}

/*
 * Generate a function call based on own signature.
 */
ProxyQuery *
plproxy_standard_query(ProxyFunction *func, bool add_types)
{
	StringInfoData sql;
	ProxyQuery *pq;
	int			i,
				len;

	pq = plproxy_func_alloc(func, sizeof(*pq));
	pq->sql = NULL;
	pq->plan = NULL;
	pq->arg_count = func->arg_count;
	len = pq->arg_count * sizeof(int);
	pq->arg_lookup = plproxy_func_alloc(func, len);

	initStringInfo(&sql);
	appendStringInfo(&sql, "select ");

	/* try to fill in all result column names */
	if (func->ret_composite)
	{
		ProxyComposite *t = func->ret_composite;
		for (i = 0; i < t->tupdesc->natts; i++)
		{
			appendStringInfo(&sql, "%s%s::%s",
							 ((i > 0) ? ", " : ""),
							 t->name_list[i],
							 t->type_list[i]->name);
		}
	}
	else
		/* names not available, do a simple query */
		appendStringInfo(&sql, "r::%s", func->ret_scalar->name);

	/* function call */
	appendStringInfo(&sql, " from %s(", func->name);

	/* fill in function arguments */
	for (i = 0; i < func->arg_count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&sql, ',');

		add_ref(&sql, i, func, i, add_types);
		pq->arg_lookup[i] = i;
	}
	appendStringInfoChar(&sql, ')');

	/*
	 * Untyped RECORD needs types specified in AS (..) clause.
	 */
	if (func->dynamic_record)
	{
		ProxyComposite *t = func->ret_composite;
		appendStringInfo(&sql, " as (");
		for (i = 0; i < t->tupdesc->natts; i++)
		{
			appendStringInfo(&sql, "%s%s %s",
							((i > 0) ? ", " : ""),
							t->name_list[i],
							t->type_list[i]->name);
		}
		appendStringInfoChar(&sql, ')');
	}

	if (func->ret_scalar)
		appendStringInfo(&sql, " r");

	pq->sql = plproxy_func_strdup(func, sql.data);
	pfree(sql.data);

	return pq;
}

/*
 * Hack to calculate split hashes with one sql.
 */
ProxyQuery *
plproxy_split_query(ProxyFunction *func, QueryBuffer *q)
{
	char *s;
	struct ArgRef *ref;
	int i, pos, fn_idx, first_split = -1;
	StringInfoData buf[1];

	if (!q->track_refs)
		elog(ERROR, "split hack needs refs");

	s = "select * from ";
	if (strncmp(q->sql->data, s, strlen(s)) == 0)
		pos = strlen(s);
	else
		pos = strlen("select ");

	initStringInfo(buf);
	appendStringInfoString(buf, "select i, ");
	for (i = 0; i < q->ref_count; i++) {
		ref = &q->refs[i];
		appendBinaryStringInfo(buf, q->sql->data + pos, ref->end - pos);
		pos = ref->end;
		fn_idx = q->arg_lookup[ref->sql_idx];
		if (func->split_args[fn_idx]) {
			appendStringInfoString(buf, "[i]");
			if (first_split < 0)
				first_split = ref->sql_idx;
		}
	}

	/* if no arrays go to hash func, add it */
	if (first_split < 0) {
		fn_idx = -1;
		for (i = 0; i < func->arg_count; i++) {
			if (func->split_args[i]) {
				fn_idx = i;
				break;
			}
		}
		if (fn_idx < 0)
			elog(ERROR, "some problem");
		first_split = q->arg_count++;
		q->arg_lookup[first_split] = fn_idx;
	}

#if PG_VERSION_NUM >= 80400
	appendStringInfo(buf, "%s from generate_subscripts($%d, 1) i",
					 q->sql->data + pos, first_split + 1);
#else
	/* 8.[23] do not have generate_subscripts() */
	appendStringInfo(buf, "%s from generate_series(array_lower($%d, 1), array_upper($%d, 1)) i",
					 q->sql->data + pos, first_split + 1, first_split + 1);
#endif

	pfree(q->sql->data);
	*q->sql = *buf;

	return plproxy_query_finish(q);
}

/*
 * Prepare ProxyQuery for local execution
 */
void
plproxy_query_prepare(ProxyFunction *func, FunctionCallInfo fcinfo, ProxyQuery *q, bool split_support)
{
	int			i;
	Oid			types[FUNC_MAX_ARGS];
	void	   *plan;

	if (func->new_split)
		split_support = false;

	/* create sql statement in sql */
	for (i = 0; i < q->arg_count; i++)
	{
		int			idx = q->arg_lookup[i];

		if (split_support && IS_SPLIT_ARG(func, idx))
			/* for SPLIT arguments use array element type instead */
			types[i] = func->arg_types[idx]->elem_type;
		else 
			types[i] = func->arg_types[idx]->type_oid;
	}

	/* prepare & store plan */
	plan = SPI_prepare(q->sql, q->arg_count, types);
	q->plan = SPI_saveplan(plan);
}

/*
 * Execute ProxyQuery locally.
 *
 * Result will be in SPI_tuptable.
 */
void
plproxy_query_exec(ProxyFunction *func, FunctionCallInfo fcinfo, ProxyQuery *q,
				   DatumArray **array_params, int array_row)
{
	int			i,
				idx,
				err;
	char		arg_nulls[FUNC_MAX_ARGS];
	Datum		arg_values[FUNC_MAX_ARGS];

	/* fill args */
	for (i = 0; i < q->arg_count; i++)
	{
		idx = q->arg_lookup[i];

		if (PG_ARGISNULL(idx))
		{
			arg_nulls[i] = 'n';
			arg_values[i] = (Datum) NULL;
		}
		else if (array_params && IS_SPLIT_ARG(func, idx))
		{
			DatumArray *ats = array_params[idx];

			arg_nulls[i] = ats->nulls[array_row] ? 'n' : ' ';
			arg_values[i] = ats->nulls[array_row] ? (Datum) NULL : ats->values[array_row];
		}
		else
		{
			arg_nulls[i] = ' ';
			arg_values[i] = PG_GETARG_DATUM(idx);
		}
	}

	/* run query */
	err = SPI_execute_plan(q->plan, arg_values, arg_nulls, true, 0);
	if (err != SPI_OK_SELECT)
		plproxy_error(func, "query '%s' failed: %s",
					  q->sql, SPI_result_code_string(err));
}

/*
 * Free cached plan.
 */
void
plproxy_query_freeplan(ProxyQuery *q)
{
	if (!q || !q->plan)
		return;
	SPI_freeplan(q->plan);
	q->plan = NULL;
}
