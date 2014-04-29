/*
   MAPI Proxy - Named properties backend MySQL implementation

   OpenChange Project

   Copyright (C) Jesús García Sáez 2014

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "namedprops_mysql.h"
#include "../mapistore.h"
#include "../mapistore_private.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>
#include <ldb.h>
#include <samba_util.h>


#define SCHEMA_FILE "named_properties_schema.sql"
#define TABLE_NAME "named_properties"


static enum mapistore_error get_mapped_id(struct namedprops_context *self,
					  struct MAPINAMEID nameid,
					  uint16_t *mapped_id)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	int type = nameid.ulKind;
	char *guid = GUID_string(mem_ctx, &nameid.lpguid);
	MYSQL *conn = self->data;

	char *sql = NULL;
	if (type == MNID_ID) {
		uint32_t prop_id = nameid.kind.lid;
		sql = talloc_asprintf(mem_ctx,
			"SELECT mappedId FROM "TABLE_NAME" "
			"WHERE `type`=%d AND `oleguid`='%s' AND `propId`=%d",
			type, guid, prop_id);
	} else if (type == MNID_STRING) {
		const char *prop_name = nameid.kind.lpwstr.Name;
		sql = talloc_asprintf(mem_ctx,
			"SELECT mappedId FROM "TABLE_NAME" "
			"WHERE `type`=%d AND `oleguid`='%s' AND `propName`='%s'",
			type, guid, prop_name);
	} else {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERROR, mem_ctx);
	}

	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}

	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND, mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);
	*mapped_id = strtol(row[0], NULL, 10);
	mysql_free_result(res);

	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}

/**
   \details Return the next unused namedprops ID

   \param nprops pointer to the namedprops context
   \param highest_id pointer to the next ID to return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
static enum mapistore_error next_unused_id(struct namedprops_context *nprops,
					   uint16_t *highest_id)
{
	TALLOC_CTX	*mem_ctx;
	MYSQL		*conn;
	MYSQL_RES	*res;
	MYSQL_ROW	row;
	char		*sql_query;
	int		ret;

	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!nprops, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!highest_id, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	conn = (MYSQL *) nprops->data;
	MAPISTORE_RETVAL_IF(!conn, MAPISTORE_ERR_DATABASE_OPS, NULL);

	mem_ctx = talloc_named(NULL, 0, "next_unused_id");
	MAPISTORE_RETVAL_IF(!mem_ctx, MAPISTORE_ERR_NO_MEMORY, NULL);

	sql_query = talloc_asprintf(mem_ctx, "SELECT max(mappedId FROM %s", TABLE_NAME);
	MAPISTORE_RETVAL_IF(!sql_query, MAPISTORE_ERR_NO_MEMORY, mem_ctx);

	ret = mysql_query(conn, sql_query);
	talloc_free(sql_query);
	MAPISTORE_RETVAL_IF(ret, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);

	res = mysql_store_result(conn);
	MAPISTORE_RETVAL_IF(!res, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);

	row = mysql_fetch_row(res);
	if (!row) {
		mysql_free_result(res);
		mapistore_set_errno(MAPISTORE_ERR_DATABASE_OPS);
		talloc_free(mem_ctx);
		return MAPISTORE_ERR_DATABASE_OPS;
	}

	*highest_id = strtol(row[0], NULL, 10);
	mysql_free_result(res);

	*highest_id = *highest_id + 1;
	return MAPISTORE_SUCCESS;
}


static enum mapistore_error create_id(struct namedprops_context *self,
				      struct MAPINAMEID nameid,
				      uint16_t mapped_id)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	const char **fields = (const char **) str_list_make_empty(mem_ctx);

	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "type=%d",
						      nameid.ulKind));
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "propType=%d",
						      PT_NULL));
	char *guid = GUID_string(mem_ctx, &nameid.lpguid);
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "oleguid='%s'",
						      guid));
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "mappedId=%u",
						      mapped_id));
	if (nameid.ulKind == MNID_ID) {
		fields = str_list_add(fields,
				      talloc_asprintf(mem_ctx, "propId=%u",
						      nameid.kind.lid));
	} else if (nameid.ulKind == MNID_STRING) {
		fields = str_list_add(fields,
				      talloc_asprintf(mem_ctx, "propName='%s'",
						      nameid.kind.lpwstr.Name));
	} else {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERROR, mem_ctx);
	}

	char *fields_sql = str_list_join(mem_ctx, fields, ',');
	char *sql = talloc_asprintf(mem_ctx,
		"INSERT INTO " TABLE_NAME " SET %s", fields_sql);
	DEBUG(5, ("Inserting record:\n%s\n", sql));
	MYSQL *conn = self->data;
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}

	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error get_nameid(struct namedprops_context *self,
				       uint16_t mapped_id,
				       TALLOC_CTX *mem_ctx,
				       struct MAPINAMEID **nameidp)
{
	TALLOC_CTX *local_mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	MYSQL *conn = self->data;
	const char *sql = talloc_asprintf(local_mem_ctx,
		"SELECT type, oleguid, propName, propId FROM "TABLE_NAME" "
		"WHERE mappedId=%d", mapped_id);
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS,
				    local_mem_ctx);
	}
	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND,
				    local_mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);

	enum mapistore_error ret = MAPISTORE_SUCCESS;
	struct MAPINAMEID *nameid = talloc_zero(mem_ctx, struct MAPINAMEID);
	const char *guid = row[1];
	GUID_from_string(guid, &nameid->lpguid);
	int type = strtol(row[0], NULL, 10);
	nameid->ulKind = type;
	if (type == MNID_ID) {
		nameid->kind.lid = strtol(row[3], NULL, 10);
	} else if (type == MNID_STRING) {
		const char *propName = row[2];
		nameid->kind.lpwstr.NameSize = strlen(propName) * 2 + 2;//FIXME WHY *2+2 and not just +1?
		nameid->kind.lpwstr.Name = talloc_strdup(nameid, propName);
	} else {
		nameid = NULL;
		ret = MAPISTORE_ERROR;
	}

	*nameidp = nameid;

	mysql_free_result(res);
	talloc_free(local_mem_ctx);

	return ret;
}

static enum mapistore_error get_nameid_type(struct namedprops_context *self,
					    uint16_t mapped_id,
					    uint16_t *prop_type)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	MYSQL *conn = self->data;
	const char *sql = talloc_asprintf(mem_ctx,
		//FIXME mappedId or propId? mappedId is not unique
		"SELECT propType FROM "TABLE_NAME" WHERE mappedId=%d",
		mapped_id);
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}
	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND, mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);
	*prop_type = strtol(row[0], NULL, 10);
	mysql_free_result(res);
	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error transaction_start(struct namedprops_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "START TRANSACTION");
	MAPISTORE_RETVAL_IF(res, MAPISTORE_ERR_DATABASE_OPS, NULL);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error transaction_commit(struct namedprops_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "COMMIT");
	MAPISTORE_RETVAL_IF(res, MAPISTORE_ERR_DATABASE_OPS, NULL);
	return MAPISTORE_SUCCESS;
}

static int mapistore_namedprops_mysql_destructor(struct namedprops_context *self)
{
	MYSQL *conn = self->data;
	mysql_close(conn);
	return 0;
}

/**
   \details Retrieve MySQL backend parametric options from
   configuration file and store them into a data structure.

   \param lp_ctx Pointer to the loadparm context
   \param p pointer to the structure with individual
   parameters to return

   \return MAPISTORE_SUCCES on success, otherwise MAPISTORE error
 */
enum mapistore_error mapistore_namedprops_mysql_parameters(struct loadparm_context *lp_ctx,
							   struct namedprops_mysql_params *p)
{
	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!lp_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!p, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Retrieve parametric options */
	p->sock = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_sock");
	p->user = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_user");
	p->pass = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_pass");
	p->host = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_host");
	p->port = lpcfg_parm_int(lp_ctx, NULL, "namedproperties", "mysql_port", 3306);
	p->db = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_db");

	/* Enforce the logic */
	MAPISTORE_RETVAL_IF(!p->user, MAPISTORE_ERR_BACKEND_INIT, NULL);
	MAPISTORE_RETVAL_IF(!p->db, MAPISTORE_ERR_BACKEND_INIT, NULL);
	MAPISTORE_RETVAL_IF(!p->host && !p->sock, MAPISTORE_ERR_BACKEND_INIT, NULL);

	return MAPISTORE_SUCCESS;
}


static bool is_schema_created(MYSQL *conn)
{
	MYSQL_RES *res = mysql_list_tables(conn, TABLE_NAME);
	if (res == NULL) return false;
	bool created = mysql_num_rows(res) == 1;
	mysql_free_result(res);
	return created;
}

static bool create_schema(MYSQL *conn)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	char *filename = talloc_asprintf(mem_ctx, "%s/" SCHEMA_FILE,
					 mapistore_namedprops_get_ldif_path());
	FILE *f = fopen(filename, "r");
	MAPISTORE_RETVAL_IF(!f, MAPISTORE_ERR_BACKEND_INIT, mem_ctx);
	fseek(f, 0, SEEK_END);
	int sql_size = ftell(f);
	rewind(f);
	char *sql = talloc_zero_array(mem_ctx, char, sql_size + 1);
	int bytes_read = fread(sql, sizeof(char), sql_size, f);
	if (bytes_read != sql_size) {
		talloc_free(mem_ctx);
		fclose(f);
		return false;
	}

	bool ret = mysql_query(conn, sql) ? false : true;

	talloc_free(mem_ctx);
	fclose(f);

	return ret;
}

static bool is_database_empty(MYSQL *conn)
{
	if (mysql_query(conn, "SELECT count(*) FROM " TABLE_NAME)) {
		// Query failed, table doesn't exist?
		return true;
	} else {
		MYSQL_RES *res = mysql_store_result(conn);
		MYSQL_ROW row = mysql_fetch_row(res);
		int n = atoi(row[0]);
		mysql_free_result(res);
		return n == 0;
	}
}

static bool add_field_from_ldif(TALLOC_CTX *mem_ctx, struct ldb_message *ldif,
				const char ***fields, const char *field,
				bool mandatory)
{
	const char *val = ldb_msg_find_attr_as_string(ldif, field, "");
	if (strlen(val) == 0) {
		if (mandatory) {
			DEBUG(0, ("%s value hasn't been found! malformed ldif?",
				  field));
		}
		return false;
	}
	char *end;
	int intval = strtol(val, &end, 10);
	if (end && strlen(val) == (end - val)) {
		*fields = str_list_add(*fields,
				       talloc_asprintf(mem_ctx, "%s=%d", field,
						       intval));
	} else {
		*fields = str_list_add(*fields,
				       talloc_asprintf(mem_ctx, "%s='%s'",
						       field, val));
	}
	return true;
}


/**
  Table fields:
    * Mandatory fields:
        * `type` TINYINT(1)
        * `propType` INT(10) unsigned
        * `oleguid` VARCHAR(255)
        * `mappedId` INT(10) unsigned
    * Optional fields:
        * `propId` INT(10) unsigned
        * `propName` VARCHAR(255)
        * `oom` VARCHAR(255)
        * `canonical` VARCHAR(255)
 */
static bool insert_ldif_msg(MYSQL *conn, struct ldb_message *ldif)
{
	const char *val = ldb_msg_find_attr_as_string(ldif, "objectClass", "");
	if (strlen(val) < strlen("MNID_ID")) {
		// It's not a valid entry, ignore it
		return true;
	}
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	const char **fields = (const char **) str_list_make_empty(mem_ctx);

	// Optional fields
	add_field_from_ldif(mem_ctx, ldif, &fields, "propId", false);
	add_field_from_ldif(mem_ctx, ldif, &fields, "propName", false);
	add_field_from_ldif(mem_ctx, ldif, &fields, "oom", false);
	add_field_from_ldif(mem_ctx, ldif, &fields, "canonical", false);
	// Mandatory fields
	// oleguid and mappedId
	if (!add_field_from_ldif(mem_ctx, ldif, &fields, "oleguid", true) ||
	    !add_field_from_ldif(mem_ctx, ldif, &fields, "mappedId", true)) {
		return false;
	}
	// type
	int type;
	if (strcmp(val, "MNID_STRING") == 0) {
		type = MNID_STRING;
	} else if (strcmp(val, "MNID_ID") == 0) {
		type = MNID_ID;
	} else {
		DEBUG(0, ("Invalid objectClass %s", val));
		return false;
	}
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "type=%d", type));
	// propType: it could be either an integer or a constant PT_*, we have
	//           to store it as an integer
	val = ldb_msg_find_attr_as_string(ldif, "propType", "");
	if (strlen(val) == 0) {
		DEBUG(0, ("propType value hasn't been found! malformed ldif?"));
		return false;
	}
	int propType;
	if (isalpha(val[0])) {
		propType = mapistore_namedprops_prop_type_from_string(val);
		if (propType == -1) {
			DEBUG(0, ("Invalid propType %s", val));
			return false;
		}
	} else {
		propType = strtol(val, NULL, 10);
	}
	fields = str_list_add(fields,
			      talloc_asprintf(mem_ctx, "propType=%d", propType));
	// Done, we have all fields on fields array
	char *fields_sql = str_list_join(mem_ctx, fields, ',');
	char *sql = talloc_asprintf(mem_ctx,
			"INSERT INTO " TABLE_NAME " SET %s", fields_sql);
	mysql_query(conn, sql);
	talloc_free(mem_ctx);
	return true;
}

static enum mapistore_error initialize_database(MYSQL *conn)
{
	if (!create_schema(conn)) {
		return MAPISTORE_ERR_DATABASE_OPS;
	}
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	struct ldb_context *ldb_ctx = ldb_init(mem_ctx, NULL);
	MAPISTORE_RETVAL_IF(!ldb_ctx, MAPISTORE_ERR_BACKEND_INIT, mem_ctx);

	char *filename = talloc_asprintf(mem_ctx, "%s/mapistore_namedprops.ldif",
					 mapistore_namedprops_get_ldif_path());
	FILE *f = fopen(filename, "r");
	MAPISTORE_RETVAL_IF(!f, MAPISTORE_ERROR, mem_ctx);

	struct ldb_ldif *ldif;
	while ((ldif = ldb_ldif_read_file(ldb_ctx, f))) {
		struct ldb_message *normalized_msg;
		int ret = ldb_msg_normalize(ldb_ctx, mem_ctx, ldif->msg,
					    &normalized_msg);
		MAPISTORE_RETVAL_IF(ret, MAPISTORE_ERR_DATABASE_INIT, mem_ctx);
		bool inserted = insert_ldif_msg(conn, normalized_msg);
		ldb_ldif_read_free(ldb_ctx, ldif);
		if (!inserted) {
			fclose(f);
			MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS,
					    mem_ctx);
		}
	}

	talloc_free(mem_ctx);
	fclose(f);

	return MAPISTORE_SUCCESS;
}


/**
   \details Initialize mapistore named properties MySQL backend

   \param mem_ctx pointer to the memory context
   \param lp_ctx pointer to the loadparm context
   \param nprops_ctx pointer on pointer to the namedprops context to
   return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
enum mapistore_error mapistore_namedprops_mysql_init(TALLOC_CTX *mem_ctx,
						     struct loadparm_context *lp_ctx,
						     struct namedprops_context **nprops_ctx)
{
	enum mapistore_error		retval;
	int				ret;
	struct namedprops_context	*nprops = NULL;
	struct namedprops_mysql_params	parms;
	MYSQL				*conn = NULL;
	MYSQL				*rconn = NULL;
	my_bool				reconnect = true;
	char				*query = NULL;

	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!lp_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!nprops_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Retrieve smb.conf arguments */
	retval = mapistore_namedprops_mysql_parameters(lp_ctx, &parms);
	if (retval) {
		DEBUG(0, ("[%s:%d] ERROR: parsing MySQL named properties "
			  "parametric option failed with %s\n",
			  __FUNCTION__, __LINE__, mapistore_errstr(retval)));
	}
	MAPISTORE_RETVAL_IF(retval, retval, NULL);

	/* Establish MySQL connection */
	conn = mysql_init(NULL);
	MAPISTORE_RETVAL_IF(!conn, MAPISTORE_ERR_NO_MEMORY, NULL);
	mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

	rconn = mysql_real_connect(conn, parms.host, parms.user, parms.pass, 
				  parms.db, parms.port, parms.sock, 0);
	if (rconn == NULL) {
		if (mysql_errno(conn) == ER_BAD_DB_ERROR) {
			DEBUG(5, ("[%s:%d] WARNING: %s\n", __FUNCTION__, __LINE__, mysql_error(conn)));
			rconn = mysql_real_connect(conn, parms.host, parms.user, parms.pass,
						   NULL, parms.port, parms.sock, 0);
			if (rconn == NULL) {
				DEBUG(0, ("[%s:%d] ERROR: %s\n", __FUNCTION__, __LINE__, mysql_error(conn)));
				return MAPISTORE_ERR_DATABASE_INIT;
			} else {
				query = talloc_asprintf(mem_ctx, "CREATE DATABASE %s", parms.db);
				ret = mysql_query(conn, query);
				MAPISTORE_RETVAL_IF(ret, MAPISTORE_ERR_DATABASE_INIT, query);
				talloc_free(query);
				
				ret = mysql_select_db(conn, parms.db);
				MAPISTORE_RETVAL_IF(ret, MAPISTORE_ERR_DATABASE_OPS, NULL);
			}
		} else {
			DEBUG(0, ("[%s:%d] ERROR: %s\n", __FUNCTION__, __LINE__, mysql_error(conn)));
			return MAPISTORE_ERR_DATABASE_INIT;
		}
	}

	/* Initialize the database */
	if ((is_schema_created(conn) == false) || (is_database_empty(conn) == true)) {
		retval = initialize_database(conn);
		MAPISTORE_RETVAL_IF(retval, retval, NULL);
	}

	/* Create context */
	nprops = talloc_zero(mem_ctx, struct namedprops_context);
	MAPISTORE_RETVAL_IF(!nprops, MAPISTORE_ERR_NO_MEMORY, NULL);

	nprops->backend_type = NAMEDPROPS_BACKEND_MYSQL;
	nprops->create_id = create_id;
	nprops->get_mapped_id = get_mapped_id;
	nprops->get_nameid = get_nameid;
	nprops->get_nameid_type = get_nameid_type;
	nprops->next_unused_id = next_unused_id;
	nprops->transaction_commit = transaction_commit;
	nprops->transaction_start = transaction_start;
	nprops->data = conn;
	talloc_set_destructor(nprops, mapistore_namedprops_mysql_destructor);

	*nprops_ctx = nprops;
	return MAPISTORE_SUCCESS;
}
