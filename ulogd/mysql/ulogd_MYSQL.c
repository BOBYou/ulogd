/* ulogd_MYSQL.c, Version $Revision: 1.1 $
 *
 * ulogd output plugin for logging to a MySQL database
 *
 * (C) 2000 by Harald Welte <laforge@gnumonks.org> 
 * This software is distributed under the terms of GNU GPL 
 *
 * $Id: ulogd_MYSQL.c,v 1.1 2000/11/20 11:43:22 laforge Exp $
 *
 **/

#include <stdlib.h>
#include <string.h>
#include <ulogd.h>
#include <mysql/mysql.h>
#include "ulogd.h"
#include "conffile.h"

#ifdef DEBUG_MYSQL
#define DEBUGP(x, args...)	fprintf(stderr, x, ## args)
#else
#define DEBUGP(x, args...)
#endif

struct _field {
	char name[ULOGD_MAX_KEYLEN];
	unsigned int id;
	struct _field *next;
};

/* the database handle we are using */
static MYSQL *dbh;

/* a linked list of the fields the table has */
static struct _field *fields;

/* buffer for our insert statement */
static char *stmt;

/* pointer to the beginning of the "VALUES" part */
static char *stmt_val;

/* pointer to current inser position in statement */
static char *stmt_ins;

/* our configuration directives */
static config_entry_t db_ce = { NULL, "mysqldb", CONFIG_TYPE_STRING,
				CONFIG_OPT_MANDATORY, 0,
				{ } };

static config_entry_t host_ce = { &db_ce, "mysqlhost", CONFIG_TYPE_STRING,
				CONFIG_OPT_MANDATORY, 0,
				{ } };

static config_entry_t user_ce = { &host_ce, "mysqluser", CONFIG_TYPE_STRING,
				CONFIG_OPT_MANDATORY, 0,
				{ } };

static config_entry_t pass_ce = { &user_ce, "mysqlpass", CONFIG_TYPE_STRING,
				CONFIG_OPT_MANDATORY, 0,
				{ } };

static config_entry_t table_ce = { &pass_ce, "mysqltable", CONFIG_TYPE_STRING,
				CONFIG_OPT_MANDATORY, 0,
				{ } };

/* is the given string a field in our table? */
static int is_field(const char *name)
{
	struct _field *f;

	for (f = fields; f; f = f->next) {
		if (!strcmp(f->name, name))
			return 1;
	}
	return 0;
}

/* our main output function, called by ulogd */
static int _mysql_output(ulog_iret_t *result)
{
	struct _field *f;
	ulog_iret_t *res;

	stmt_ins = stmt_val;

	for (f = fields; f; f = f->next) {
		res = keyh_getres(f->id);

		if (!res) {
			ulogd_log(ULOGD_NOTICE,
				"no result for %s ?!?\n", f->name);
		}
			
		if (!res || !IS_VALID((*res))) {
			/* no result, we have to fake something */
			sprintf(stmt_ins, "NULL,");
			stmt_ins = stmt + strlen(stmt);
			continue;
		}
		
		switch (res->type) {
			case ULOGD_RET_INT8:
				sprintf(stmt_ins, "%d,", res->value.i8);
				break;
			case ULOGD_RET_INT16:
				sprintf(stmt_ins, "%d,", res->value.i16);
				break;
			case ULOGD_RET_INT32:
				sprintf(stmt_ins, "%d,", res->value.i32);
				break;
			case ULOGD_RET_INT64:
				sprintf(stmt_ins, "%ld,", res->value.i64);
				break;
			case ULOGD_RET_UINT8:
				sprintf(stmt_ins, "%u,", res->value.ui8);
				break;
			case ULOGD_RET_UINT16:
				sprintf(stmt_ins, "%u,", res->value.ui16);
				break;
			case ULOGD_RET_IPADDR:
			case ULOGD_RET_UINT32:
				sprintf(stmt_ins, "%u,", res->value.ui32);
				break;
			case ULOGD_RET_UINT64:
				sprintf(stmt_ins, "%lu,", res->value.ui64);
				break;
			case ULOGD_RET_STRING:
				*stmt_ins++ = '\'';
				mysql_real_escape_string(dbh, stmt_ins,
					res->value.ptr, strlen(res->value.ptr));
				stmt_ins = stmt + strlen(stmt);
				sprintf(stmt_ins, "',");
			/* sprintf(stmt_ins, "'%s',", res->value.ptr); */
				break;
			default:
				ulogd_log(ULOGD_NOTICE,
					"unknown type %d for %s\n",
					res->type, res->key);
				break;
		}
		stmt_ins = stmt + strlen(stmt);
	}
	*(stmt_ins - 1) = ')';
	DEBUGP("stmt=#%s#\n", stmt);

	/* now we have created our statement, insert it */

	if(mysql_real_query(dbh, stmt, strlen(stmt))) {
		ulogd_log(ULOGD_ERROR, "sql error during insert: %s\n",
				mysql_error(dbh));
		return 1;
	}

	return 0;
}

#define MYSQL_INSERTTEMPL   "insert into X (Y) values (Z)"
#define MYSQL_VALSIZE	100

/* create the static part of our insert statement */
static int _mysql_createstmt(void)
{
	struct _field *f;
	unsigned int size;
	char buf[ULOGD_MAX_KEYLEN];
	char *underscore;

	if (stmt) {
		ulogd_log(ULOGD_NOTICE, "createstmt called, but stmt"
			" already existing\n");	
		return 1;
	}

	/* caclulate the size for the insert statement */
	size = strlen(MYSQL_INSERTTEMPL) + strlen(table_ce.u.string);

	for (f = fields; f; f = f->next) {
		/* we need space for the key and a comma, as well as
		 * enough space for the values */
		size += strlen(f->name) + 1 + MYSQL_VALSIZE;
	}	

	ulogd_log(ULOGD_DEBUG, "allocating %u bytes for statement\n", size);

	stmt = (char *) malloc(size);

	if (!stmt) {
		ulogd_log(ULOGD_ERROR, "OOM!\n");
		return 1;
	}

	sprintf(stmt, "insert into %s (", table_ce.u.string);
	stmt_val = stmt + strlen(stmt);

	for (f = fields; f; f = f->next) {
		strncpy(buf, f->name, ULOGD_MAX_KEYLEN);	
		while (underscore = strchr(buf, '.'))
			*underscore = '_';
		sprintf(stmt_val, "%s,", buf);
		stmt_val = stmt + strlen(stmt);
	}
	*(stmt_val - 1) = ')';

	sprintf(stmt_val, " values (");
	stmt_val = stmt + strlen(stmt);

	ulogd_log(ULOGD_DEBUG, "stmt='%s'\n", stmt);

	return 0;
}

/* find out which columns the table has */
static int _mysql_get_columns(const char *table)
{
	MYSQL_RES *result;
	MYSQL_FIELD *field;
	char buf[ULOGD_MAX_KEYLEN];
	char *underscore;
	struct _field *f;
	int id;

	if (!dbh) 
		return 1;

	result = mysql_list_fields(dbh, table, NULL);
	if (!result)
		return 1;

	while (field = mysql_fetch_field(result)) {

		/* replace all underscores with dots */
		strncpy(buf, field->name, ULOGD_MAX_KEYLEN);
		while (underscore = strchr(buf, '_'))
			*underscore = '.';

		DEBUGP("field '%s' found: ", buf);

		if (!(id = keyh_getid(buf))) {
			DEBUGP(" no keyid!\n");
			continue;
		}

		DEBUGP("keyid %u\n", id);

		/* prepend it to the linked list */
		f = (struct _field *) malloc(sizeof *f);
		if (!f) {
			ulogd_log(ULOGD_ERROR, "OOM!\n");
			return 1;
		}
		strncpy(f->name, buf, ULOGD_MAX_KEYLEN);
		f->id = id;
		f->next = fields;
		fields = f;	
	}

	mysql_free_result(result);
	return 0;
}

/* make connection and select database */
static int _mysql_open_db(char *server, char *user, char *pass, char *db)
{
	dbh = mysql_connect(NULL, server, user, pass);

	if (!dbh)
		return 1;

	mysql_select_db(dbh, db);
	return 0;
}

static ulog_output_t _mysql_plugin = { NULL, "mysql", &_mysql_output };

void _init(void) 
{
	/* register our configfile options here */
	config_register_key(&table_ce);

	/* have the opts parsed */
	config_parse_file(0);

	if (_mysql_open_db(host_ce.u.string, user_ce.u.string, 
			   pass_ce.u.string, db_ce.u.string)) {
		ulogd_log(ULOGD_ERROR, "can't establish database connection\n");
		return;
	}

	/* read the fieldnames to know which values to insert */
	if (_mysql_get_columns(table_ce.u.string)) {
		ulogd_log(ULOGD_ERROR, "unable to get mysql columns\n");
		return;
	}
	_mysql_createstmt();
	register_output(&_mysql_plugin);

}