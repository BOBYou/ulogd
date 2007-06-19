/* ulogd_MYSQL.c, Version $Revision$
 *
 * ulogd output plugin for logging to a MySQL database
 *
 * (C) 2000-2001 by Harald Welte <laforge@gnumonks.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 
 *  as published by the Free Software Foundation
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 * $Id$
 *
 * 15 May 2001, Alex Janssen <alex@ynfonatic.de>:
 *      Added a compability option for older MySQL-servers, which
 *      don't support mysql_real_escape_string
 *
 * 17 May 2001, Alex Janssen <alex@ynfonatic.de>:
 *      Added the --with-mysql-log-ip-as-string feature. This will log
 *      IP's as string rather than an unsigned long integer to the database.
 *	See ulogd/doc/mysql.table.ipaddr-as-string as an example.
 *	BE WARNED: This has _WAY_ less performance during table searches.
 *
 * 09 Feb 2005, Sven Schuster <schuster.sven@gmx.de>:
 * 	Added the "port" parameter to specify ports different from 3306
 *
 * 12 May 2005, Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *	Added reconnecting to lost mysql server.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <ulogd/ulogd.h>
#include <ulogd/conffile.h>
#include <mysql/mysql.h>
#include <inttypes.h>

#ifdef DEBUG_MYSQL
#define DEBUGP(x, args...)	fprintf(stderr, x, ## args)
#else
#define DEBUGP(x, args...)
#endif

struct _field {
	char name[ULOGD_MAX_KEYLEN];
	unsigned int id;
	unsigned int str;
	struct _field *next;
};

/* The plugin handler */
static ulog_output_t mysql_plugin;

/* the database handle we are using */
static MYSQL *dbh;

/* a linked list of the fields the table has */
static struct _field *fields;

/* buffer for our insert statement */
static char *stmt;

/* size of our insert statement buffer */
static size_t stmt_siz;

/* pointer to the beginning of the "VALUES" part */
static char *stmt_val;

/* pointer to current inser position in statement */
static char *stmt_ins;

#define STMT_ADD(fmt...) \
	do { 								      \
		if (stmt_ins >= stmt && stmt_siz > stmt_ins - stmt)            \
			snprintf(stmt_ins, stmt_siz-(stmt_ins-stmt), ##fmt); \
	} while(0)

/* Attempt to reconnect if connection is lost */
time_t reconnect = 0;
#define TIME_ERR		((time_t)-1)	/* Be paranoid */

/* our configuration directives */
static config_entry_t db_ce = { 
	.key = "db", 
	.type = CONFIG_TYPE_STRING,
	.options = CONFIG_OPT_MANDATORY,
};

static config_entry_t host_ce = { 
	.next = &db_ce, 
	.key = "host", 
	.type = CONFIG_TYPE_STRING,
	.options = CONFIG_OPT_MANDATORY,
};

static config_entry_t user_ce = { 
	.next = &host_ce, 
	.key = "user", 
	.type = CONFIG_TYPE_STRING,
	.options = CONFIG_OPT_MANDATORY,
};

static config_entry_t pass_ce = { 
	.next = &user_ce, 
	.key = "pass", 
	.type = CONFIG_TYPE_STRING,
	.options = CONFIG_OPT_MANDATORY,
};

static config_entry_t table_ce = { 
	.next = &pass_ce, 
	.key = "table", 
	.type = CONFIG_TYPE_STRING,
	.options = CONFIG_OPT_MANDATORY,
};

static config_entry_t port_ce = {
	.next = &table_ce,
	.key = "port",
	.type = CONFIG_TYPE_INT,
};

static config_entry_t reconnect_ce = {
	.next = &port_ce,
	.key = "reconnect",
	.type = CONFIG_TYPE_INT,
};

static config_entry_t connect_timeout_ce = {
	.next = &reconnect_ce,
	.key = "connect_timeout",
	.type = CONFIG_TYPE_INT,
};

static int _mysql_init_db(ulog_iret_t *result);
static void _mysql_fini(void);

/* our main output function, called by ulogd */
static int mysql_output(ulog_iret_t *result)
{
	struct _field *f;
	ulog_iret_t *res;
#ifdef IP_AS_STRING
	char *tmpstr;		/* need this for --log-ip-as-string */
	struct in_addr addr;
#endif
	size_t esclen;

	if (stmt_val == NULL) {
		_mysql_fini();
		return _mysql_init_db(result);
	}

	stmt_ins = stmt_val;

	for (f = fields; f; f = f->next) {
		res = keyh_getres(f->id);

		if (!res) {
			ulogd_log(ULOGD_NOTICE,
				"no result for %s ?!?\n", f->name);
		}
			
		if (!res || !IS_VALID((*res))) {
			/* no result, we have to fake something */
			STMT_ADD("NULL,");
			stmt_ins = stmt + strlen(stmt);
			continue;
		}
		
		switch (res->type) {
			case ULOGD_RET_INT8:
				STMT_ADD("%d,", res->value.i8);
				break;
			case ULOGD_RET_INT16:
				STMT_ADD("%d,", res->value.i16);
				break;
			case ULOGD_RET_INT32:
				STMT_ADD("%d,", res->value.i32);
				break;
			case ULOGD_RET_INT64:
				STMT_ADD("%"PRId64",", res->value.i64);
				break;
			case ULOGD_RET_UINT8:
				STMT_ADD("%u,", res->value.ui8);
				break;
			case ULOGD_RET_UINT16:
				STMT_ADD("%u,", res->value.ui16);
				break;
			case ULOGD_RET_IPADDR:
#ifdef IP_AS_STRING
				if (f->str) {
					addr.s_addr = ntohl(res->value.ui32);
					tmpstr = inet_ntoa(addr);
					esclen = (strlen(tmpstr)*2) + 4;
					if (stmt_siz <= (stmt_ins-stmt)+esclen){
						STMT_ADD("'',");
						break;
					}

					*stmt_ins++ = '\'';
#ifdef OLD_MYSQL
					mysql_escape_string(stmt_ins,
							    tmpstr,
							    strlen(tmpstr));
#else
					mysql_real_escape_string(dbh, 
								stmt_ins,
								tmpstr,
								strlen(tmpstr));
#endif /* OLD_MYSQL */
	                                stmt_ins = stmt + strlen(stmt);
					STMT_ADD("',");
					break;
				}
#endif /* IP_AS_STRING */
				/* EVIL: fallthrough when logging IP as
				 * u_int32_t */
			case ULOGD_RET_UINT32:
				STMT_ADD("%u,", res->value.ui32);
				break;
			case ULOGD_RET_UINT64:
				STMT_ADD("%"PRIu64",", res->value.ui64);
				break;
			case ULOGD_RET_BOOL:
				STMT_ADD("'%d',", res->value.b);
				break;
			case ULOGD_RET_STRING:
				esclen = (strlen(res->value.ptr)*2) + 4;
				if (stmt_siz <= (stmt_ins-stmt) + esclen) {
					STMT_ADD("'',");
					break;
				}
				*stmt_ins++ = '\'';
#ifdef OLD_MYSQL
				mysql_escape_string(stmt_ins, res->value.ptr,
					strlen(res->value.ptr));
#else
				mysql_real_escape_string(dbh, stmt_ins,
					res->value.ptr, strlen(res->value.ptr));
#endif
				stmt_ins = stmt + strlen(stmt);
				STMT_ADD("',");
				break;
			case ULOGD_RET_RAW:
				ulogd_log(ULOGD_NOTICE,
					"%s: type RAW not supported by MySQL\n",
					res->key);
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
	*stmt_ins = '\0';

	DEBUGP("stmt=#%s#\n", stmt);

	/* now we have created our statement, insert it */

	if (mysql_real_query(dbh, stmt, strlen(stmt))) {
		ulogd_log(ULOGD_ERROR, "sql error during insert: %s\n",
				mysql_error(dbh));
		_mysql_fini();
		return _mysql_init_db(result);
	}

	return 0;
}

/* no connection, plugin disabled */
static int mysql_output_disabled(ulog_iret_t *result)
{
	return 0;
}

#define MYSQL_INSERTTEMPL   "insert into X (Y) values (Z)"
#define MYSQL_VALSIZE	100

/* create the static part of our insert statement */
static int mysql_createstmt(void)
{
	struct _field *f;
	char buf[ULOGD_MAX_KEYLEN];
	char *underscore;

	if (stmt)
		free(stmt);

	/* caclulate the size for the insert statement */
	stmt_siz = strlen(MYSQL_INSERTTEMPL) + strlen(table_ce.u.string) + 1;

	for (f = fields; f; f = f->next) {
		/* we need space for the key and a comma, as well as
		 * enough space for the values */
		stmt_siz += strlen(f->name) + 1 + MYSQL_VALSIZE;
	}	

	ulogd_log(ULOGD_DEBUG, "allocating %zu bytes for statement\n",
				stmt_siz);

	stmt = (char *) malloc(stmt_siz);

	if (!stmt) {
		stmt_val = NULL;
		stmt_siz = 0;
		ulogd_log(ULOGD_ERROR, "OOM!\n");
		return -1;
	}

	snprintf(stmt, stmt_siz, "insert into %s (", table_ce.u.string);
	stmt_val = stmt + strlen(stmt);

	for (f = fields; f; f = f->next) {
		strncpy(buf, f->name, ULOGD_MAX_KEYLEN-1);
		buf[ULOGD_MAX_KEYLEN-1] = '\0';
		while ((underscore = strchr(buf, '.')))
			*underscore = '_';
		STMT_ADD(stmt_val,stmt,stmt_siz, "%s,", buf);
		stmt_val = stmt + strlen(stmt);
	}
	*(stmt_val - 1) = ')';

	STMT_ADD(stmt_val,stmt,stmt_siz, " values (");
	stmt_val = stmt + strlen(stmt);

	ulogd_log(ULOGD_DEBUG, "stmt='%s'\n", stmt);

	return 0;
}

/* find out which columns the table has */
static int mysql_get_columns(const char *table)
{
	MYSQL_RES *result;
	MYSQL_FIELD *field;
	char buf[ULOGD_MAX_KEYLEN];
	char *underscore;
	struct _field *f;
	int id;

	if (!dbh) 
		return -1;

	result = mysql_list_fields(dbh, table, NULL);
	if (!result)
		return -1;

	/* Cleanup before reconnect */
	while (fields) {
		f = fields;
		fields = f->next;
		free(f);
	}

	while ((field = mysql_fetch_field(result))) {

		/* replace all underscores with dots */
		strncpy(buf, field->name, ULOGD_MAX_KEYLEN-1);
		buf[ULOGD_MAX_KEYLEN-1] = '\0';

		while ((underscore = strchr(buf, '_')))
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
			return -1;
		}
		strncpy(f->name, buf, ULOGD_MAX_KEYLEN-1);
		f->name[ULOGD_MAX_KEYLEN-1] = '\0';
		f->id = id;
		f->str = !IS_NUM(field->type);
		f->next = fields;
		fields = f;	
	}

	mysql_free_result(result);
	return 0;
}

/* make connection and select database */
static int mysql_open_db(char *server, int port, char *user, char *pass, 
			 char *db)
{
	dbh = mysql_init(NULL);
	if (!dbh)
		return -1;

	if (connect_timeout_ce.u.value)
		mysql_options(dbh, MYSQL_OPT_CONNECT_TIMEOUT,
			(const char *) &connect_timeout_ce.u.value);

	if (!mysql_real_connect(dbh, server, user, pass, db, port, NULL, 0))
	{
		_mysql_fini();
		return -1;
	}

	return 0;
}

static int init_reconnect(void)
{
	if (reconnect_ce.u.value) {
		reconnect = time(NULL);
		if (reconnect != TIME_ERR) {
			ulogd_log(ULOGD_ERROR, "no connection to database, "
					       "attempting to reconnect "
					       "after %u seconds\n",
					       reconnect_ce.u.value);
			reconnect += reconnect_ce.u.value;
			mysql_plugin.output = &_mysql_init_db;
			return -1;
		}
	}
	/* Disable plugin permanently */
	mysql_plugin.output = &mysql_output_disabled;
	
	return 0;
}

static int _mysql_init_db(ulog_iret_t *result)
{
	if (reconnect && reconnect > time(NULL))
		return 0;
	
	if (mysql_open_db(host_ce.u.string, port_ce.u.value, user_ce.u.string, 
			   pass_ce.u.string, db_ce.u.string)) {
		ulogd_log(ULOGD_ERROR, "can't establish database connection\n");
		return init_reconnect();
	}

	/* read the fieldnames to know which values to insert */
	if (mysql_get_columns(table_ce.u.string)) {
		ulogd_log(ULOGD_ERROR, "unable to get mysql columns\n");
		_mysql_fini();
		return init_reconnect();
	}

	if (mysql_createstmt())
	{
		ulogd_log(ULOGD_ERROR, "unable to create mysql statement\n");
		_mysql_fini();
		return init_reconnect();
	}

	/* enable plugin */
	mysql_plugin.output = &mysql_output;

	reconnect = 0;

	if (result)
		return mysql_output(result);
		
	return 0;
}

static int _mysql_init(void)
{
	/* have the opts parsed */
	config_parse_file("MYSQL", &connect_timeout_ce);

	return _mysql_init_db(NULL);
}

static void _mysql_fini(void)
{
	if (dbh) {
		mysql_close(dbh);
		dbh = NULL;
	}
}

static ulog_output_t mysql_plugin = { 
	.name = "mysql", 
	.output = &mysql_output, 
	.init = &_mysql_init,
	.fini = &_mysql_fini,
};

void _init(void) 
{
	register_output(&mysql_plugin);
}
