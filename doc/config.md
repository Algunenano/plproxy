
# PL/Proxy Cluster Configuration

PL/Proxy can be used in either CONNECT mode or CLUSTER mode.

In CONNECT mode PL/Proxy acts as a pass through proxy to another database.
Each PL/Proxy function contains a libpq connect string for the connection
to a database it will proxy the request to.

PL/Proxy can also be used in CLUSTER mode where it provides support for
partitioning data across multiple databases based on a clustering function.

When using PL/Proxy in CONNECT mode no special configuration is required.
However, using PL/Proxy in CLUSTER mode requires the cluster configuration
to be defined, either by the cluster configuration API or SQL/MED.

## Cluster configuration API

The following plproxy schema functions are used to define the clusters:

### plproxy.get_cluster_version(cluster_name)

    plproxy.get_cluster_version(cluster_name text)
    returns integer

The `get_cluster_version()` function is called on each request, it should return 
the version number of the current configuration for a particular cluster.  
If the version number returned by this function is higher than the one plproxy 
has cached, then the configuration and partition information will be reloaded
by calling the `get_cluster_config()` and `get_cluster_partitions()` functions.

This is an example function that does not lookup the version number for an 
external source such as a configuration table.

    CREATE OR REPLACE FUNCTION plproxy.get_cluster_version(cluster_name text)
    RETURNS int4 AS $$
    BEGIN
	IF cluster_name = 'a_cluster' THEN
            RETURN 1;
        END IF;
        RAISE EXCEPTION 'Unknown cluster';
    END;
    $$ LANGUAGE plpgsql;


### plproxy.get_cluster_partitions(cluster_name)

    plproxy.get_cluster_partitions(cluster_name text)
    returns setof text

This is called when a new partition configuration needs to be loaded. 
It should return connect strings to the partitions in the cluster.
The connstrings should be returned in the correct order.  The total
number of connstrings returned must be a power of 2.  If two or more
connstrings are equal then they will use the same connection.

If the string `user=` does not appear in a connect string then
`user=CURRENT_USER` will be appended to the connection string by PL/Proxy.  
This will cause PL/Proxy to connect to the partition database using
the same username as was used to connect to the proxy database.
This also avoids the accidental `user=postgres` connections.

There are several approaches how to handle passwords:

* Store passwords in `.pgpass` or `pg_service.conf`.  Secure.
  (unless you have dblink installed on same Postgres instance.)
  Only problem is that it's not administrable from inside the database.

* Load per-user password from table/file and append it to connect string.
  Slight problem - users can see the password.

* Use single user/password for all users and put it into connect string.
  Bigger problem - users can see the password.

* Use **trust** authentication on a pooler listening on locahost/unix socket.
  This is good combination with PgBouncer as it can load
  passwords directly from Postgres own `pg_auth` file and
  use them for remote connections.

* Use **trust** authentication on remote database.  Obviously bad idea.

An example function without the use of separate configuration tables:

    CREATE OR REPLACE FUNCTION plproxy.get_cluster_partitions(cluster_name text)
    RETURNS SETOF text AS $$
    BEGIN
        IF cluster_name = 'a_cluster' THEN
            RETURN NEXT 'dbname=part00 host=127.0.0.1';
            RETURN NEXT 'dbname=part01 host=127.0.0.1';
            RETURN NEXT 'dbname=part02 host=127.0.0.1';
            RETURN NEXT 'dbname=part03 host=127.0.0.1';
            RETURN;
        END IF;
        RAISE EXCEPTION 'Unknown cluster';
    END;
    $$ LANGUAGE plpgsql;

### plproxy.get_cluster_config(cluster)
 
    plproxy.get_cluster_config(
            IN cluster_name text,
            OUT key text,
            OUT val text)
    RETURNS SETOF record

The `get_cluster_config()` function returns a set of key-value pairs that can 
consist of any of the following configuration parameters.  All of them are 
optional. Timeouts/lifetime values are given in seconds.  If the value is 0
or NULL then the parameter is disabled (a default value will be used).


* `connection_lifetime`

  The maximum age a connection (in seconds) to a remote database will be kept
  open for. If this is disabled (0) then connections to remote databases will 
  be kept open as long as they are valid. Otherwise once a connection reaches 
  the age indicated it will be closed.

* `query_timeout`

  If a query result does not appear in this time, the connection
  is closed.  If set then `statement_timeout` should also be set
  on remote server to a somewhat smaller value, so it takes effect earlier.
  It is meant for surviving network problems, not long queries.

* `disable_binary`

  Do not use binary I/O for connections to this cluster.

* `keepalive_idle`

  TCP keepalive - how long the connection needs to be idle,
  before keepalive packets can be sent.  In seconds.

* `keepalive_interval`

  TCP keepalive - interval between keepalive packets.  In seconds.

* `keepalive_count`

  TCP keepalive - how many packets to send.  If none get answer,
  connection will be close.

* `connect_timeout`

  Initial connect is canceled, if it takes more that this.

  **Deprecated**: it duplicates libpq connect string parameter
  with same name.  Its better to just add the parameter to
  connect string.

* `default_user`

  Either `current_user` (default) or `session_user`.  They have same
  meaning as SQL tokens.  The specified user is used to look up SQL/MED
  user mapping.  In case of non-SQL/MED cluster, the user is put directly
  to connect string, unless there already exist `user=` key.  The user is
  also used to cache the connections.  Thus PL/Proxy 2.4+ supports connecting
  to single cluster from same backend with different users.

  **Deprecated**: it's use is to restore pre-2.4 default of `session_user`.

Example function without the use of separate tables for storing parameters.

    CREATE OR REPLACE FUNCTION plproxy.get_cluster_config(
        IN cluster_name text,
        OUT key text,
        OUT val text)
    RETURNS SETOF record AS $$
    BEGIN
        -- lets use same config for all clusters
        key := 'connection_lifetime';
        val := 30*60; -- 30m
        RETURN NEXT;
        RETURN;
    END;
    $$ LANGUAGE plpgsql;

## SQL/MED cluster definitions

Pl/Proxy can take advantage of SQL/MED connection info management available
in PostgreSQL 8.4 and above. The benefits of using SQL/MED are simplified
cluster definition management and slightly improved performance.

Both SQL/MED defined clusters and configuration function based clusters can
coexist in the same database. If a cluster is defined in both, SQL/MED takes
precedence. If no SQL/MED cluster is found an attempt is made to fall back to
configuration functions.

### Configuring SQL/MED clusters

First we need to create a foreign data wrapper. Generally the FDW is a kind of
driver that provides remote database access, data marshalling etc. In this
case, it's main role is to provide a validation function that sanity checks
your cluster definitions.

Note: the validation function is known to be broken in PostgreSQL 8.4.2 and
below.


    CREATE FOREIGN DATA WRAPPER plproxy
            [ VALIDATOR plproxy_fdw_validator ]
            [ OPTIONS global options ] ;

Next we need to define a CLUSTER, this is done by creating a SERVER that uses
the plproxy FDW.  The options to the SERVER are PL/Proxy configuration settings
and the list of cluster partitions.

Note: USAGE access to the SERVER must be explicitly granted. Without this,
users are unable to use the cluster.

    CREATE SERVER a_cluster FOREIGN DATA WRAPPER plproxy
            OPTIONS (
                    connection_lifetime '1800',
                    disable_binary '1',
                    p0 'dbname=part00 host=127.0.0.1',
                    p1 'dbname=part01 host=127.0.0.1',
                    p2 'dbname=part02 host=127.0.0.1',
                    p3 'dbname=part03 host=127.0.0.1'
                    );

Finally we need to create a user mapping for the Pl/Proxy users. One might
create individual mappings for specific users:

    CREATE USER MAPPING FOR bob SERVER a_cluster OPTIONS (user 'bob', password 'secret');

or create a PUBLIC mapping for all users of the system:

    CREATE USER MAPPING FOR public SERVER a_cluster OPTIONS (user 'plproxy', password 'foo');

Also it is possible to create both individual and PUBLIC mapping, in this case
the individual mapping takes precedence.

