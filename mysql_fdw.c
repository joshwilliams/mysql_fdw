/*-------------------------------------------------------------------------
 *
 *		  foreign-data wrapper for MySQL
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Dave Page <dpage@pgadmin.org>
 *
 * IDENTIFICATION
 *		  mysql_fdw/mysql_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define list_length mysql_list_length
#define list_delete mysql_list_delete
#define list_free mysql_list_free
#include <mysql.h>
#undef list_length
#undef list_delete
#undef list_free

#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "parser/parse_coerce.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct MySQLFdwOption
{
	const char	*optname;
	Oid		optcontext;	/* Oid of catalog in which option may appear */
};

/*
 * Valid options for mysql_fdw.
 *
 */
static struct MySQLFdwOption valid_options[] =
{

	/* Connection options */
	{ "address",		ForeignServerRelationId },
	{ "port",		ForeignServerRelationId },
	{ "username",		UserMappingRelationId },
	{ "password",		UserMappingRelationId },
	{ "database",		ForeignTableRelationId },
	{ "query",		ForeignTableRelationId },
	{ "table",		ForeignTableRelationId },

	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */

typedef struct MySQLFdwExecutionState
{
	MYSQL		*conn;			/* MySQL connection object */
	MYSQL_RES	*result;		/* MySQL result set handler */
	char		*query;			/* query string */
	unsigned int num_fields;	/* how many fields the query returns */
} MySQLFdwExecutionState;

/*
 * SQL functions
 */
extern Datum mysql_fdw_handler(PG_FUNCTION_ARGS);
extern Datum mysql_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(mysql_fdw_handler);
PG_FUNCTION_INFO_V1(mysql_fdw_validator);

/*
 * FDW callback routines
 */
static FdwPlan *mysqlPlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel);
static void mysqlExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void mysqlBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *mysqlIterateForeignScan(ForeignScanState *node);
static void mysqlReScanForeignScan(ForeignScanState *node);
static void mysqlEndForeignScan(ForeignScanState *node);

/*
 * Helper functions
 */
static bool mysqlIsValidOption(const char *option, Oid context);
static void mysqlGetOptions(Oid foreigntableid, char **address, int *port, char **username, char **password, char **database, char **query, char **table);

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
mysql_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->PlanForeignScan = mysqlPlanForeignScan;
	fdwroutine->ExplainForeignScan = mysqlExplainForeignScan;
	fdwroutine->BeginForeignScan = mysqlBeginForeignScan;
	fdwroutine->IterateForeignScan = mysqlIterateForeignScan;
	fdwroutine->ReScanForeignScan = mysqlReScanForeignScan;
	fdwroutine->EndForeignScan = mysqlEndForeignScan;

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
mysql_fdw_validator(PG_FUNCTION_ARGS)
{
	List		*options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid		catalog = PG_GETARG_OID(1);
	char		*svr_address = NULL;
	int		svr_port = 0;
	char		*svr_username = NULL;
	char		*svr_password = NULL;
	char		*svr_database = NULL;
	char		*svr_query = NULL;
	char		*svr_table = NULL;
	ListCell	*cell;

	/*
	 * Check that only options supported by mysql_fdw,
	 * and allowed for the current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);

		if (!mysqlIsValidOption(def->defname, catalog))
		{
			struct MySQLFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
							 opt->optname);
			}

			ereport(ERROR, 
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME), 
				errmsg("invalid option \"%s\"", def->defname), 
				errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
				));
		}

		if (strcmp(def->defname, "address") == 0)
		{
			if (svr_address)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), 
					errmsg("conflicting or redundant options: address (%s)", defGetString(def))
					));

			svr_address = defGetString(def);
		}
		else if (strcmp(def->defname, "port") == 0)
		{
			if (svr_port)
				ereport(ERROR, 
					(errcode(ERRCODE_SYNTAX_ERROR), 
					errmsg("conflicting or redundant options: port (%s)", defGetString(def))
					));

			svr_port = atoi(defGetString(def));
		}
		if (strcmp(def->defname, "username") == 0)
		{
			if (svr_username)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: username (%s)", defGetString(def))
					));

			svr_username = defGetString(def);
		}
		if (strcmp(def->defname, "password") == 0)
		{
			if (svr_password)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: password")
					));

			svr_password = defGetString(def);
		}
		else if (strcmp(def->defname, "database") == 0)
		{
			if (svr_database)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: database (%s)", defGetString(def))
					));

			svr_database = defGetString(def);
		}
		else if (strcmp(def->defname, "query") == 0)
		{
			if (svr_table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting options: query cannot be used with table")
					));

			if (svr_query)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: query (%s)", defGetString(def))
					));

			svr_query = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (svr_query)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting options: table cannot be used with query")
					));

			if (svr_table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: table (%s)", defGetString(def))
					));

			svr_table = defGetString(def);
		}
	}

	PG_RETURN_VOID();
}


/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
mysqlIsValidOption(const char *option, Oid context)
{
	struct MySQLFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a mysql_fdw foreign table.
 */
static void
mysqlGetOptions(Oid foreigntableid, char **address, int *port, char **username, char **password, char **database, char **query, char **table)
{
	ForeignTable	*f_table;
	ForeignServer	*f_server;
	UserMapping	*f_mapping;
	List		*options;
	ListCell	*lc;

	/*
	 * Extract options from FDW objects.
	 */
	f_table = GetForeignTable(foreigntableid);
	f_server = GetForeignServer(f_table->serverid);
	f_mapping = GetUserMapping(GetUserId(), f_table->serverid);

	options = NIL;
	options = list_concat(options, f_table->options);
	options = list_concat(options, f_server->options);
	options = list_concat(options, f_mapping->options);

	/* Loop through the options, and get the server/port */
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "address") == 0)
			*address = defGetString(def);

		if (strcmp(def->defname, "port") == 0)
			*port = atoi(defGetString(def));

		if (strcmp(def->defname, "username") == 0)
			*username = defGetString(def);

		if (strcmp(def->defname, "password") == 0)
			*password = defGetString(def);

		if (strcmp(def->defname, "database") == 0)
			*database = defGetString(def);

		if (strcmp(def->defname, "query") == 0)
			*query = defGetString(def);

		if (strcmp(def->defname, "table") == 0)
			*table = defGetString(def);
	}

	/* Default values, if required */
	if (!*address)
		*address = "127.0.0.1";

	if (!*port)
		*port = 3306;

	/* Check we have the options we need to proceed */
	if (!*table && !*query)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("either a table or a query must be specified")
			));
}

/*
 * mysqlPlanForeignScan
 *		Create a FdwPlan for a scan on the foreign table
 */
static FdwPlan *
mysqlPlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel)
{
	FdwPlan		*fdwplan;
	char		*svr_address = NULL;
	int		svr_port = 0;
	char		*svr_username = NULL;
	char		*svr_password = NULL;
	char 		*svr_database = NULL;
	char 		*svr_query = NULL;
	char 		*svr_table = NULL;
	char		*query;
	double		rows = 0;
	MYSQL	   *conn;
	MYSQL_RES	*result;
	MYSQL_ROW	row;

	/* Fetch options  */
	mysqlGetOptions(foreigntableid, &svr_address, &svr_port,
					&svr_username, &svr_password,
					&svr_database, &svr_query, &svr_table);

	/* Construct FdwPlan with cost estimates. */
	fdwplan = makeNode(FdwPlan);

	/* Local databases are probably faster */
	if (strcmp(svr_address, "127.0.0.1") == 0 || strcmp(svr_address, "localhost") == 0)
		fdwplan->startup_cost = 10;
	else
		fdwplan->startup_cost = 25;

	/*
	 * TODO: Find a way to stash this connection object away, so we don't have
	 * to reconnect to MySQL again later.
	 */

	/* Connect to the server */
	conn = mysql_init(NULL);
	if (!conn)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
			errmsg("failed to initialise the MySQL connection object")
			));

	mysql_options(conn, MYSQL_SET_CHARSET_NAME, GetDatabaseEncodingName());

	if (!mysql_real_connect(conn, svr_address, svr_username, svr_password,
							svr_database, svr_port, NULL,
							CLIENT_COMPRESS | CLIENT_REMEMBER_OPTIONS ))
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
			errmsg("failed to connect to MySQL: %s", mysql_error(conn))
			));

	/* Build the query */
	if (svr_query)
	{
		size_t len = strlen(svr_query) + 9;

		query = (char *) palloc(len);
		snprintf(query, len, "EXPLAIN %s", svr_query);
	}
	else
	{
		size_t len = strlen(svr_table) + 23;

		query = (char *) palloc(len);
		snprintf(query, len, "EXPLAIN SELECT * FROM %s", svr_table);
	}

	/*
	 * MySQL seems to have some pretty unhelpful EXPLAIN output, which only
	 * gives a row estimate for each relation in the statement. We'll use the
	 * sum of the rows as our cost estimate - it's not great (in fact, in some
	 * cases it sucks), but it's all we've got for now.
	 */
	if (mysql_query(conn, query) != 0)
	{
		char *err = pstrdup(mysql_error(conn));
		mysql_close(conn);
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			errmsg("failed to execute the MySQL query: %s", err)
			));
	}

	/*
	 * http://dev.mysql.com/doc/refman/5.0/en/mysql-use-result.html
	 * http://dev.mysql.com/doc/refman/5.0/en/null-mysql-store-result.html
	 *
	 * We assume we're given a query that does return data (SELECT).
	 */
	result = mysql_store_result(conn);

	if (result == NULL)
	{
		char *err = pstrdup(mysql_error(conn));
		mysql_close(conn);
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			errmsg("failed to execute the MySQL query: %s", err)));
	}

	while ((row = mysql_fetch_row(result)))
		rows += atof(row[8]);

	mysql_free_result(result);
	mysql_close(conn);

	baserel->rows = rows;
	baserel->tuples = rows;
	fdwplan->total_cost = rows + fdwplan->startup_cost;
	fdwplan->fdw_private = NIL;	/* not used */

	return fdwplan;
}

/*
 * fileExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
mysqlExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char		    *svr_address = NULL;
	int		     svr_port = 0;
	char		    *svr_username = NULL;
	char		    *svr_password = NULL;
	char		    *svr_database = NULL;
	char		    *svr_query = NULL;
	char		    *svr_table = NULL;

	MySQLFdwExecutionState *festate = (MySQLFdwExecutionState *) node->fdw_state;

	/* Fetch options  */
	mysqlGetOptions(RelationGetRelid(node->ss.ss_currentRelation), &svr_address, &svr_port, &svr_username, &svr_password, &svr_database, &svr_query, &svr_table);

	/* Give some possibly useful info about startup costs */
	if (es->costs)
	{
		if (strcmp(svr_address, "127.0.0.1") == 0 || strcmp(svr_address, "localhost") == 0)	
			ExplainPropertyLong("Local server startup cost", 10, es);
		else
			ExplainPropertyLong("Remote server startup cost", 25, es);
		ExplainPropertyText("MySQL query", festate->query, es);
	}
}

/*
 * mysqlBeginForeignScan
 *		Initiate access to the database
 */
static void
mysqlBeginForeignScan(ForeignScanState *node, int eflags)
{
	char			*svr_address = NULL;
	int			svr_port = 0;
	char			*svr_username = NULL;
	char			*svr_password = NULL;
	char			*svr_database = NULL;
	char			*svr_query = NULL;
	char			*svr_table = NULL;
	MYSQL			*conn;
	MySQLFdwExecutionState  *festate;
	char			*query;

	/* Fetch options  */
	mysqlGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
					&svr_address, &svr_port,
					&svr_username, &svr_password,
					&svr_database, &svr_query, &svr_table);

	/* Connect to the server */
	conn = mysql_init(NULL);
	if (!conn)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
			errmsg("failed to initialise the MySQL connection object")
			));

	mysql_options(conn, MYSQL_SET_CHARSET_NAME, GetDatabaseEncodingName());

	if (!mysql_real_connect(conn, svr_address, svr_username, svr_password,
							svr_database, svr_port, NULL,
							CLIENT_COMPRESS | CLIENT_REMEMBER_OPTIONS ))
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
			errmsg("failed to connect to MySQL: %s", mysql_error(conn))
			));

	/* Build the query */
	if (svr_query)
		query = svr_query;
	else
	{
		size_t len = strlen(svr_table) + 15;

		query = (char *)palloc(len);
		snprintf(query, len, "SELECT * FROM %s", svr_table);
	}

	/* Stash away the state info we have already */
	festate = (MySQLFdwExecutionState *) palloc(sizeof(MySQLFdwExecutionState));
	node->fdw_state = (void *) festate;
	festate->conn = conn;
	festate->result = NULL;
	festate->query = query;
	festate->num_fields = 0;
}

/*
 * mysqlVerifymbstr
 *
 * We don't trust MySQL to implement the PostgreSQL encoding names exactly the
 * same (I guess that would depend also on the MySQL server's version), so we
 * recheck that here before we go to store the value in the TupleStore.
 *
 * Note that we only call that function when we have a column whose type
 * category is some string.
 *
 * Instead of erroring out when the data is not compliant with the database
 * encoding, we raise a WARNING and return false here, and return NULL at the
 * upper level, so that it's still possible to use this driver for MySQL data
 * migrations.
 *
 * FIXME: a way to export this choice as a SERVER options might be good.
 */
static bool
mysqlVerifymbstr(const char *mbstr, int len)
{
	if (!pg_verifymbstr(mbstr, len, true))
	{
		int			l = pg_encoding_mblen(GetDatabaseEncoding(), mbstr);
		char		buf[8 * 5 + 1];
		char	   *p = buf;
		int			j, jlimit;

		jlimit = Min(l, len);
		jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

		for (j = 0; j < jlimit; j++)
		{
			p += sprintf(p, "0x%02x", (unsigned char) mbstr[j]);
			if (j < jlimit - 1)
				p += sprintf(p, " ");
		}
		ereport(WARNING,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid byte sequence for encoding \"%s\": %s",
						GetDatabaseEncodingName(),
						mbstr)));
		return false;
	}
	return true;
}
/*
 * mysqlIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot *
mysqlIterateForeignScan(ForeignScanState *node)
{
	MYSQL_ROW		row;

	MySQLFdwExecutionState *festate = (MySQLFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	AttInMetadata *meta =
		TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);

	/* Execute the query, if required */
	if (!festate->result)
	{
		if (mysql_query(festate->conn, festate->query) != 0)
		{
			char *err = pstrdup(mysql_error(festate->conn));
			mysql_close(festate->conn);
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("failed to execute the MySQL query: %s", err)));
		}

		/*
		 * http://dev.mysql.com/doc/refman/5.0/en/mysql-use-result.html
		 * http://dev.mysql.com/doc/refman/5.0/en/null-mysql-store-result.html
		 *
		 * We assume we're given a query that does return data (SELECT).
		 */
		festate->result = mysql_store_result(festate->conn);

		if (festate->result == NULL)
		{
			char *err = pstrdup(mysql_error(festate->conn));
			mysql_close(festate->conn);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("failed to execute the MySQL query: %s", err)));
		}

		/* remember the field count, that doesn't change mid-query */
		festate->num_fields = mysql_num_fields(festate->result);
	}

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple. If we don't find another row, we just skip the
	 * last step, leaving the slot empty as required.
	 */
	ExecClearTuple(slot);

	/* Get the next tuple */
	if ((row = mysql_fetch_row(festate->result)))
	{
		Datum	   *dvalues;
		bool	   *nulls;
		unsigned long    *lengths;
		int				  x, y;

		nulls = (bool *) palloc(festate->num_fields * sizeof(bool));
		dvalues = (Datum *) palloc(festate->num_fields * sizeof(Datum));
		lengths = mysql_fetch_lengths(festate->result);

		for (x = y = 0; x < festate->num_fields; x++, y++)
		{
			/* Handle dropped attributes by setting to NULL */
			while (meta->tupdesc->attrs[y]->attisdropped)
			{
				elog(NOTICE, "attisdropped[%d]", y);

				dvalues[y] = (Datum) 0;
				nulls[y] = true;
				y++;
			}

			if (row[x] == NULL)
			{
				/* we have a NULL value here */
				dvalues[y] = (Datum) 0;
				nulls[y] = true;
			}
			else if (lengths[x] == 0)
			{
				/* special case empty string input */
				nulls[y] = false;
				dvalues[y] = InputFunctionCall(&meta->attinfuncs[y],
											   "",
											   meta->attioparams[y],
											   meta->atttypmods[y]);
			}
			else
			{
				bool encoding_error = false;
				if (TypeCategory(meta->attioparams[y]) == TYPCATEGORY_STRING)
					encoding_error = ! mysqlVerifymbstr(row[x], lengths[x]);

				if (encoding_error)
				{
					nulls[y] = true;
					dvalues[y] = (Datum) 0;
				}
				else
				{
					nulls[y] = false;
					dvalues[y] = InputFunctionCall(&meta->attinfuncs[y],
												   row[x],
												   meta->attioparams[y],
												   meta->atttypmods[y]);
				}
			}
		}
		slot->tts_isnull = nulls;
		slot->tts_values = dvalues;
		ExecStoreVirtualTuple(slot);
	}
	return slot;
}

/*
 * mysqlEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
mysqlEndForeignScan(ForeignScanState *node)
{
	MySQLFdwExecutionState *festate = (MySQLFdwExecutionState *) node->fdw_state;

	if (festate->result)
	{
		mysql_free_result(festate->result);
		festate->result = NULL;
	}

	if (festate->conn)
	{
		mysql_close(festate->conn);
		festate->conn = NULL;
	}

	if (festate->query)
	{
		pfree(festate->query);
		festate->query = 0;
	}
}

/*
 * mysqlReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
mysqlReScanForeignScan(ForeignScanState *node)
{
	MySQLFdwExecutionState *festate = (MySQLFdwExecutionState *) node->fdw_state;

	if (festate->result)
	{
		mysql_data_seek(festate->result, 0);
	}
}

