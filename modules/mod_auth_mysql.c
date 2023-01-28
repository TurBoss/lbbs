/*
 * LBBS -- The Lightweight Bulletin Board System
 *
 * Copyright (C) 2023, Naveen Albert
 *
 * Naveen Albert <bbs@phreaknet.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief MySQL-backed user authentication and registration
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

/*!
 * \note The SQL interface really belongs in its own file, I just hesitate
 * to move it into the BBS core and mandate that the entire BBS core
 * be linked with the MySQL/MariaDB library.
 * For now, this contains that linking requirement to this one module,
 * which is technically optional if somebody wants to use an alternate one.
 * Might make sense to have a mod_sql module that exposes SQL functions
 * with global symbols?
 */

/*
 * Full MySQL/MariaDB C API documentation available here:
 * https://mariadb.com/kb/en/mariadb-connectorc-api-functions/
 */
#include <mysql/mysql.h>

#include "include/module.h"
#include "include/config.h"
#include "include/auth.h"
#include "include/user.h"
#include "include/node.h" /* needed for user registration */
#include "include/term.h" /* needed for user registration */
#include "include/crypt.h" /* use bbs_password_verify_bcrypt */
#include "include/utils.h" /* use bbs_str_isprint */

static char buf_dbhostname[32];
static char buf_dbusername[32];
static char buf_dbpassword[32];
static char buf_dbname[32] = "";
/* strlen_zero doesn't like being called directly on char buffers, need pointers */
static char *dbhostname = buf_dbhostname;
static char *dbusername = buf_dbusername;
static char *dbpassword = buf_dbpassword;
static char *dbname = buf_dbname;

#define DB_NAME_ARGS dbname, !strlen_zero(dbname) ? "." : ""

#define log_mysqli_error(mysql) bbs_error("mysql error %d [%s]: %s\n", mysql_errno(mysql), mysql_sqlstate(mysql), mysql_error(mysql))

static MYSQL *sql_connect(void)
{
	MYSQL *mysql;

	if (strlen_zero(dbhostname) || strlen_zero(dbusername) || strlen_zero(dbpassword)) {
		bbs_error("One or more necessary DB config options is missing\n"); /* ^ DB name is optional.... */
		return NULL;
	}
	if (!(mysql = mysql_init(NULL))) {
		bbs_error("mysql_init returned NULL\n");
		return NULL;
	}
	if (mysql_optionsv(mysql, MYSQL_SET_CHARSET_NAME, (void *) "utf8")) {
		goto fail;
	}
	if (!mysql_real_connect(mysql, dbhostname, dbusername, dbpassword, dbname, 0, NULL, 0)) {
		goto fail;
	}
	if (mysql_set_character_set(mysql, "utf8")) { /* Make sure that mysql_real_escape_string can always do its job. */
		goto fail;
	}
	return mysql;

fail:
	log_mysqli_error(mysql);
	mysql_close(mysql);
	return NULL;
}

static int sql_prepare(MYSQL_STMT *stmt, const char *fmt, const char *query)
{
	int i, qlen;
	int num_args;
	const char *cur = fmt;

	if (!stmt) {
		bbs_warning("failed to get stmt\n");
		return -1;
	}
	if (strlen_zero(query)) {
		bbs_warning("No query provided\n");
		return -1;
	}
	if (strlen_zero(fmt)) {
		bbs_warning("No argument format string provided\n");
		return -1;
	}

	num_args = strlen(fmt);
	qlen = strlen(query);

	if (mysql_stmt_prepare(stmt, query, qlen)) {
		bbs_warning("mysql_stmt_prepare failed: %s (%s)\n", mysql_stmt_error(stmt), query);
		return -1;
	}
	if ((int) mysql_stmt_param_count(stmt) != num_args) {
		bbs_warning("Expected %d parameters but prepared %lu?\n", num_args, mysql_stmt_param_count(stmt));
		return -1;
	}

	/* No point in really doing much at this point... just check that our format string is good. */
	for (i = 0; i < num_args; i++) {
		switch (*cur) {
		/* Supported */
		case 'i': /* Integer */
		case 'l': /* Long */
		case 'd': /* Double */
		case 's': /* String */
		case 't': /* Date */
			break;
		/* Not supported */
		case 'b': /* Blob */
		default:
			bbs_warning("Unsupported SQL format type specifier: %c\n", *cur);
			return -1;
		}
		cur++;
	}

	return 0;
}

static int sql_string_prep(int num_fields, char *bind_strings[], unsigned long int lengths[], int index, int size)
{
	if (index >= num_fields) {
		bbs_warning("Index is out of bounds: %d\n", index);
		return -1;
	}
	lengths[index] = size;
	bind_strings[index] = malloc(lengths[index]);
	if (!bind_strings[index]) {
		bbs_error("malloc failure\n");
		return -1;
	}
	return 0;
}

static void sql_free_result_strings(int num_fields, unsigned long int lengths[], char *bind_strings[])
{
	int i;
	for (i = 0; i < num_fields; i++) {
		if (lengths[i] && bind_strings[i]) {
			free(bind_strings[i]);
			bind_strings[i] = NULL;
		}
	}
}

static int sql_bind_param_single(va_list ap, int i, const char *cur, MYSQL_BIND bind[], unsigned long int lengths[], int bind_ints[], long long bind_longs[], char *bind_strings[], MYSQL_TIME bind_dates[], my_bool bind_null[])
{
	struct tm *tm;
	char format_char = tolower(*cur);
	bind_null[i] = (my_bool) isupper(format_char); /* Uppercase format char means it's NULL */

#if 0
	bbs_debug(10, "Executing fmt char: %c\n", format_char);
#endif

	/* Ref: https://dev.mysql.com/doc/c-api/5.7/en/c-api-prepared-statement-data-structures.html */
	/* See for C <-> MySQL/MariaDB types: https://dev.mysql.com/doc/c-api/5.7/en/c-api-prepared-statement-type-codes.html */
	switch (format_char) {
	case 'i': /* Integer */
		bind_ints[i] = va_arg(ap, int);
		/* This is a number type, so there is no need to specify buffer_length */
		bind[i].buffer_type = MYSQL_TYPE_LONG; /* Yes, this is correct */
		bind[i].buffer = (char *) &bind_ints[i];
		bind[i].is_null = &bind_null[i];
		bind[i].length = 0;
		break;
	case 'l': /* Long int */
		bind_longs[i] = va_arg(ap, long long);
		/* This is a number type, so there is no need to specify buffer_length */
		bind[i].buffer_type = MYSQL_TYPE_LONGLONG;
		bind[i].buffer = (char *) &bind_longs[i];
		bind[i].is_null = &bind_null[i];
		bind[i].length = 0;
		break;
	case 'd': /* Double */
		bind_ints[i] = va_arg(ap, int);
		/* This is a number type, so there is no need to specify buffer_length */
		bind[i].buffer_type = MYSQL_TYPE_LONG;
		bind[i].buffer = (char *) &bind_ints[i];
		bind[i].is_null = &bind_null[i];
		bind[i].length = 0;
		break;
	case 's': /* String */
		bind_strings[i] = va_arg(ap, char *);
		lengths[i] = strlen(S_IF(bind_strings[i]));
		if (!bind_strings[i] && !bind_null[i]) {
			bbs_warning("String at index %d is NULL, but not specified?\n", i);
		}
		bind[i].buffer_type = MYSQL_TYPE_STRING;
		bind[i].buffer = (char *) bind_strings[i];
		bind[i].buffer_length = lengths[i];
		bind[i].is_null = &bind_null[i];
		bind[i].length = &lengths[i]; /* For strings, we actually do need the length. We'll be able to find it in the array. */
		break;

/* see ctime(3) */
#define TM_MONTH(m) (m + 1)
#define TM_YEAR(y) (y + 1900)

	case 't': /* Date */
		tm = va_arg(ap, struct tm *);
		bind_dates[i].year = TM_YEAR(tm->tm_year);
		bind_dates[i].month = TM_MONTH(tm->tm_mon);
		bind_dates[i].day = tm->tm_mday;
		bind_dates[i].hour = tm->tm_hour;
		bind_dates[i].minute = tm->tm_min;
		bind_dates[i].second = tm->tm_sec;
		bind[i].buffer_type = MYSQL_TYPE_DATE;
		bind[i].buffer = (char *) &bind_dates[i];
		bind[i].is_null = &bind_null[i];
		bind[i].length = 0;
		break;
	case 'b': /* Blob */
		bbs_warning("Blobs are currently unsupported\n");
		return -1;
	default:
		bbs_warning("Unknown SQL format type specifier: %c\n", *cur);
		return -1;
	}
	return 0;
}

#pragma GCC diagnostic ignored "-Wstack-protector"
static int sql_prep_bind_exec(MYSQL_STMT *stmt, const char *query, const char *fmt, ...)
{
	int i, num_args = strlen(fmt);
	va_list ap;
	MYSQL_BIND bind[num_args];
	unsigned long int lengths[num_args];
	/* We need an array of the size of the num args, for every type... not sure if there's a better way, but not really a big deal... */
	int bind_ints[num_args];
	long long bind_longs[num_args];
	char *bind_strings[num_args];
	my_bool bind_null[num_args];
	MYSQL_TIME bind_dates[num_args];
	const char *cur = fmt;
#pragma GCC diagnostic pop

	if (sql_prepare(stmt, fmt, query)) {
		return -1;
	}

	memset(bind, 0, sizeof(bind));
	memset(bind_dates, 0, sizeof(bind_dates)); ////

	va_start(ap, fmt); 
	for (i = 0; i < num_args; i++, cur++) { /* Bind the parameters themselves for this round */
		if (sql_bind_param_single(ap, i, cur, bind, lengths, bind_ints, bind_longs, bind_strings, bind_dates, bind_null)) {
			va_end(ap);
			return -1;
		}
	}
	va_end(ap);

	/* Finish it off, go in for the execute */
	if (mysql_stmt_bind_param(stmt, bind)) { /* Bind the buffers */
		bbs_error("mysql_stmt_bind_param failed: %s\n", mysql_stmt_error(stmt));
		return -1;
	}
	if (mysql_stmt_execute(stmt)) {
		bbs_error("mysql_stmt_execute failed: %s\n", mysql_stmt_error(stmt));
		return -1;
	}
	return 0;
}

/* If we use sql_alloc_bind_strings instead of sql_string_prep in advance, then the result will initially be truncated, until we call sql_alloc_bind_strings */
/* if mysql_stmt_fetch returns 1 or MYSQL_NO_DATA, break */
#define MYSQL_NEXT_ROW(stmt) (!(mysqlres = sql_stmt_fetch(stmt)) || (mysqlres == MYSQL_DATA_TRUNCATED))

static int sql_stmt_fetch(MYSQL_STMT *stmt)
{
	int res = mysql_stmt_fetch(stmt);
	switch (res) {
	case 0: /* Success */
		break;
	case 1: /* Failure */
		bbs_error("SQL STMT fetch failed: %s\n", mysql_stmt_error(stmt));
		break;
	case MYSQL_NO_DATA:
		bbs_debug(3, "SQL STMT fetch returned no more data\n");
		break;
	case MYSQL_DATA_TRUNCATED:
		bbs_debug(3, "SQL STMT fetch data truncated\n"); /* Caller needs to allocate bigger buffer(s), *OR* this is okay, if we're using sql_alloc_bind_strings */
		break;
	default:
		bbs_error("Unexpected SQL STMT fetch return code: %d\n", res);
	}
	return res;
}

/*! \brief Automatically allocate any memory needed to hold string results */
/*! \note You must call sql_stmt_fetch BEFORE calling this */
static int sql_alloc_bind_strings(MYSQL_STMT *stmt, const char *fmt, MYSQL_BIND bind[], unsigned long int lengths[], char *bind_strings[])
{
	int i, res = 0;
	int nullstrings = 0;
	const char *cur = fmt;
	int num_cols = strlen(fmt);

	for (i = 0; i < num_cols; i++) {
		switch (*cur++) {
		case 's': /* String */
			if (bind[i].buffer_length == 0 && lengths[i] > 0) {
				nullstrings++;
			}
			break;
		default:
			break;
		}
	}

	if (nullstrings) {
		/* If we have a string and the buffer length is 0, dynamically allocate memory now. */
		bbs_debug(5, "%d string field%s must be dynamically allocated\n", nullstrings, ESS(nullstrings));
		for (i = 0; i < num_cols; i++) {
			if (bind[i].buffer_type == MYSQL_TYPE_STRING && bind[i].buffer == NULL && lengths[i] > 0) {
				res = -1;
				bbs_debug(6, "Allocating dynamic buffer at index %d for string of length %lu\n", i, lengths[i]);
				bind[i].buffer = calloc(1, lengths[i] + 1); /* Add 1 for null terminator, even though MySQL won't add one. */
				if (!bind[i].buffer) {
					bbs_error("malloc failed\n");
					lengths[i] = 0; /* Set back to 0 so we don't attempt to free unallocated memory in sql_free_result_strings */
				} else {
					bind_strings[i] = bind[i].buffer; /* Make sure we have a reference to the allocated memory */
					bind[i].buffer_length = lengths[i];
					/* The official documentation for this function has a typo in it that has never been corrected: https://dev.mysql.com/doc/c-api/8.0/en/mysql-stmt-fetch.html
					 * See: https://bugs.mysql.com/bug.php?id=33086
					 * If there's one thing I really hate, it's documentation that is wrong or not maintained... argh... */
					res = mysql_stmt_fetch_column(stmt, &bind[i], i, 0);
					if (res) { /* ith column, offset 0 to start at beginning */
						bbs_error("mysql_stmt_fetch_column(%d) failed (%d): %s\n", i, res, mysql_stmt_error(stmt));
						/* Free now since this buffer is useless anyways */
						free(bind_strings[i]);
						bind_strings[i] = NULL;
						lengths[i] = 0;
					} else {
						if (strlen(bind_strings[i]) != lengths[i]) {
							bbs_warning("Column %d: expected length %lu but have %lu\n", i, lengths[i], strlen(bind_strings[i]));
						} else {
							res = 0;
						}
						/* No need to null terminate here since we used calloc above */
					}
				}
			}
		}
	} else {
		bbs_warning("No string fields need to be dynamically allocated, this function invocation was unnecessary!\n"); /* No harm done, but not necessary */
	}
	return res;
}

static int sql_bind_result(MYSQL_STMT *stmt, const char *fmt, MYSQL_BIND bind[], unsigned long int lengths[], int bind_ints[], char *bind_strings[], my_bool bind_null[])
{
	int num_cols, num_rows, expect_cols;
	int i, res = -1;
	MYSQL_RES *prepare_meta_result;
	const char *cur = fmt;

	prepare_meta_result = mysql_stmt_result_metadata(stmt);
	if (!prepare_meta_result) {
		bbs_warning("mysql_stmt_result_metadata returned NULL: %s\n", mysql_stmt_error(stmt));
		return -1;
	}

	expect_cols = strlen(fmt);
	num_cols = mysql_num_fields(prepare_meta_result);

	/* Ensure number of columns in results is what we expected */
	if (num_cols != expect_cols) {
		bbs_warning("Expected %d columns but got %d?\n", expect_cols, num_cols);
		goto cleanup;
	}

	for (i = 0; i < num_cols; i++) {
		switch (*cur++) {
		case 'i': /* Integer */
			/* This is a number type, so there is no need to specify buffer_length */
			bind[i].buffer_type = MYSQL_TYPE_LONG; /* Yes, this is correct */
			bind[i].buffer = (char *) &bind_ints[i];
			bind[i].is_null = &bind_null[i];
			bind[i].length = 0;
			break;
		case 'd': /* Double */
			/* This is a number type, so there is no need to specify buffer_length */
			bind[i].buffer_type = MYSQL_TYPE_LONG;
			bind[i].buffer = (char *) &bind_ints[i];
			bind[i].is_null = &bind_null[i];
			bind[i].length = 0;
			break;
		case 's': /* String */
			bind[i].buffer_type = MYSQL_TYPE_STRING;
			bind[i].buffer = (char *) bind_strings[i];
			bind[i].buffer_length = lengths[i];
			bind[i].is_null = &bind_null[i];
			bind[i].length = &lengths[i]; /* For strings, we actually do need the length. We'll be able to find it in the array. */
			break;
		case 'b': /* Blob */
			bbs_warning("Blobs are currently unsupported\n");
			goto cleanup;
		case 'l': /*! \todo Not supported here yet, because it would break the API, and we haven't needed l for this func yet, so kicking the can down the road... */
		default:
			bbs_warning("Unknown SQL format type specifier: %c\n", *cur);
			goto cleanup;
		}
	}

	/* Bind result buffers */
	if (mysql_stmt_bind_result(stmt, bind)) {
		bbs_warning("mysql_stmt_bind_result failed: %s\n", mysql_stmt_error(stmt));
		goto cleanup;
	}

	/* Don't call sql_alloc_bind_strings here, because then we've already fetched the first row of results before we enter the while loop.
	 * And generally speaking, sql_alloc_bind_strings needs to be called (and the allocated strings freed) once per row,
	 * so that's the more general way to handle it. */

	if (mysql_stmt_store_result(stmt)) {
		bbs_warning("mysql_stmt_store_result failed: %s\n", mysql_stmt_error(stmt));
		goto cleanup;
	}

	num_rows = mysql_stmt_num_rows(stmt);
	bbs_debug(10, "Query returned %d rows\n", num_rows);

	res = 0;
cleanup:
	mysql_free_result(prepare_meta_result);
	return res;
}

/*!
 * \brief Attempt to authenticate user from MySQL/MariaDB database
 * \param user BBS user struct
 * \param username
 * \param password
 * \retval 0 on successful login, -1 on failure
 */
#pragma GCC diagnostic ignored "-Wstack-protector"
static int provider(AUTH_PROVIDER_PARAMS)
{
	char sql[128];
	const unsigned int num_fields = 5;
	MYSQL *mysql = NULL;
	MYSQL_STMT *stmt;
	int founduser = 0, res = -1;
	int mysqlres;
	const char *fmt = "dssds";

	mysql = sql_connect();
	NULL_RETURN(mysql);

	stmt = mysql_stmt_init(mysql);
	if (!stmt) {
		goto cleanup;
	}

	snprintf(sql, sizeof(sql), "SELECT id, username, password, priv, email FROM %s%susers WHERE username = ? LIMIT 1", DB_NAME_ARGS);

	if (sql_prep_bind_exec(stmt, sql, "s", username)) {
		goto cleanup;
	} else {
		/* Indented a block since we need num_fields */
		MYSQL_BIND results[num_fields]; /* should be equal to number of selected cols */
		unsigned long int lengths[num_fields]; /* Only needed for string result fields */
		int bind_ints[num_fields];
		char *bind_strings[num_fields];
		my_bool bind_null[num_fields];
#pragma GCC diagnostic pop

		memset(results, 0, sizeof(results));
		memset(lengths, 0, sizeof(lengths));
		memset(bind_strings, 0, sizeof(bind_strings));

#if 0
		/* Initialize our only string fields for storing a result. */
		if (sql_string_prep(num_fields, bind_strings, lengths, 1, 24)) { /* username */
			goto stmtcleanup; /* Bail out on malloc failures */
		}
		if (sql_string_prep(num_fields, bind_strings, lengths, 2, 84)) { /* password (bcrypt len = 60) */
			goto stmtcleanup; /* Bail out on malloc failures */
		}
		if (sql_string_prep(num_fields, bind_strings, lengths, 4, 84)) { /* email */
			goto stmtcleanup; /* Bail out on malloc failures */
		}
#endif

		if (sql_bind_result(stmt, fmt, results, lengths, bind_ints, bind_strings, bind_null)) {
			goto stmtcleanup;
		}

		/* XXX this works but the next MYSQL_NEXT_ROW call fails with commands out of sync - this problem may have previously existed though */
		while (MYSQL_NEXT_ROW(stmt)) {
			int id, priv;
			char *real_username, *pw_hash, *email;

			id = bind_ints[0];
			priv = bind_ints[3];

			/* Must allocate string results before attempting to use them */
			if (sql_alloc_bind_strings(stmt, fmt, results, lengths, bind_strings)) { /* Needs to be called if we don't use sql_string_prep in advance for all strings. */
				break; /* If we fail for some reason, don't crash attempting to access NULL strings */
			}
			real_username = bind_strings[1];
			pw_hash = bind_strings[2];
			email = bind_strings[4];

			founduser++;
			if (!bbs_password_verify_bcrypt(password, pw_hash)) { /* XXX We're explicitly assuming here that the hashes are bcrypt hashes */
				res = 0;
				/* Set user info */
				user->id = id;
				if (user->username) {
					/* XXX Why would this ever be non-NULL here? */
					bbs_warning("Already had a username?\n");
					free_if(user->username);
				}
				user->username = strdup(real_username);
				user->priv = priv;
				user->email = email ? strdup(email) : NULL;
				bbs_debug(3, "Successful password auth for %s\n", real_username);
				/* XXX First, retrieve last login before this (before we update) */
				/* Update last_login timestamp to NOW */
				snprintf(sql, sizeof(sql), "UPDATE %s%susers SET last_login = NOW() WHERE username = ? LIMIT 1", DB_NAME_ARGS);
				if (!sql_prep_bind_exec(stmt, sql, "s", username)) {
					bbs_debug(6, "Updated last_login timestamp\n");
				} else {
					bbs_warning("Failed to update last_login timestamp\n");
				}
			} else {
				bbs_debug(3, "Failed password auth for %s\n", real_username);
			}
			sql_free_result_strings(num_fields, lengths, bind_strings); /* Call inside the while loop, since strings only need to be freed per row */
		}

stmtcleanup:
		sql_free_result_strings(num_fields, lengths, bind_strings); /* Won't hurt anything, clean up in case we break from the loop */
		mysql_stmt_close(stmt);
		if (!founduser) {
			/* If we didn't find a user, do a dummy call to bbs_password_verify_bcrypt
			 * to prevent timing attacks (user exists or doesn't exist) */
#define DUMMY_PASSWORD "P@ssw0rd123"
#define DUMMY_PASSWORD_HASH "$2y$10$0uZL6ZrlTFw1Z.pyKPOLXub2cQdrRAPMAuHz0gWsmzwy4W/6oOLt2"
			bbs_password_verify_bcrypt(DUMMY_PASSWORD, DUMMY_PASSWORD_HASH);
#undef DUMMY_PASSWORD
#undef DUMMY_PASSWORD_HASH
		}
	}

cleanup:
	mysql_close(mysql);
	return res;
}

/* XXX This is very similar to provider, except we're just filling in the user struct without doing a password check */
#pragma GCC diagnostic ignored "-Wstack-protector"
static struct bbs_user *get_user_info(const char *username)
{
	char sql[128];
	const unsigned int num_fields = 4;
	MYSQL *mysql = NULL;
	MYSQL_STMT *stmt;
	struct bbs_user *user;

	mysql = sql_connect();
	if (!mysql) {
		return NULL;
	}

	stmt = mysql_stmt_init(mysql);
	if (!stmt) {
		goto cleanup;
	}

	snprintf(sql, sizeof(sql), "SELECT id, username, priv, email FROM %s%susers WHERE username = ? LIMIT 1", DB_NAME_ARGS);

	if (sql_prep_bind_exec(stmt, sql, "s", username)) {
		goto cleanup;
	} else {
		/* Indented a block since we need num_fields */
		MYSQL_BIND results[num_fields]; /* should be equal to number of selected cols */
		unsigned long int lengths[num_fields]; /* Only needed for string result fields */
		int bind_ints[num_fields];
		char *bind_strings[num_fields];
		my_bool bind_null[num_fields];
#pragma GCC diagnostic pop

		memset(results, 0, sizeof(results));
		memset(lengths, 0, sizeof(lengths));
		memset(bind_strings, 0, sizeof(bind_strings));

		/* Initialize our only string fields for storing a result. */
		if (sql_string_prep(num_fields, bind_strings, lengths, 1, 24)) { /* username */
			goto stmtcleanup; /* Bail out on malloc failures */
		}
		if (sql_string_prep(num_fields, bind_strings, lengths, 3, 84)) { /* email */
			goto stmtcleanup; /* Bail out on malloc failures */
		}

		if (sql_bind_result(stmt, "dsds", results, lengths, bind_ints, bind_strings, bind_null)) {
			goto stmtcleanup;
		}

		/* if mysql_stmt_fetch returns 1 or MYSQL_NO_DATA, break */
		while (!mysql_stmt_fetch(stmt)) {
			int id, priv;
			char *real_username = bind_strings[1], *email = bind_strings[3];
			id = bind_ints[0];
			priv = bind_ints[2];
			user = bbs_user_request();
			if (!user) {
				break;
			}
			/* Set user info */
			user->id = id;
			user->username = strdup(real_username);
			user->priv = priv;
			user->email = email ? strdup(email) : NULL;
		}

stmtcleanup:
		sql_free_result_strings(num_fields, lengths, bind_strings);
		mysql_stmt_close(stmt);
	}

cleanup:
	mysql_close(mysql);
	return user;
}

static int change_password(const char *username, const char *password)
{
	char pw_hash[61];
	MYSQL *mysql = NULL;
	MYSQL_STMT *stmt;
	int res = -1;
	char sql[96];
	const char *types = "ss";

	if (bbs_password_salt_and_hash(password, pw_hash, sizeof(pw_hash))) {
		return -1;
	}

	/* We expect that the users table has a UNIQUE constraint on the username column
	 * Columns like date_registered and priv should be set automatically on INSERT. */
	snprintf(sql, sizeof(sql), "UPDATE %s%susers SET password = ? WHERE username = ?", DB_NAME_ARGS);

	mysql = sql_connect();
	NULL_RETURN(mysql);
	stmt = mysql_stmt_init(mysql);
	if (!stmt || sql_prep_bind_exec(stmt, sql, types, pw_hash, username)) { /* Bind parameters and execute */
		goto cleanup;
	}
	/* XXX Do we still return 0 even if we updated 0 records? If so, should we return -1 instead? */
	res = 0;

cleanup:
	if (stmt) {
		mysql_stmt_close(stmt);
	}
	mysql_close(mysql);
	return res;
}

static int invalid_birthday(struct tm *tm)
{
	struct tm nowtime;
	time_t timenow = time(NULL);

	gmtime_r(&timenow, &nowtime);

	bbs_debug(3, "Analyzing date: %d/%d/%d\n", TM_MONTH(tm->tm_mon), tm->tm_mday, TM_YEAR(tm->tm_year));

	/* Can't be older than the oldest person alive or younger than now. Even this is very conservative, how many infants and centenarians are BBSing? */
	if (TM_YEAR(tm->tm_year) < 1903 || tm->tm_year > nowtime.tm_year) {
		bbs_debug(3, "Year not valid: %d\n", TM_YEAR(tm->tm_year));
		return -1;
	}
	return 0;
}

static int make_user(const char *username, const char *password, const char *fullname, const char *email, const char *phone,
	const char *address, const char *city, const char *state, const char *zip, const char *dob, char gender)
{
	char pw_hash[61];
	MYSQL *mysql = NULL;
	MYSQL_STMT *stmt;
	int res = -1;
	char sql[184];
	char genderbuf[2] = { gender, '\0' }; /* We can't pass a char directly into sql_prep_bind_exec, we must pass a char* */
	struct tm birthday;
	const char *types = "sssssssssts";

	memset(&birthday, 0, sizeof(birthday));

	if (bbs_password_salt_and_hash(password, pw_hash, sizeof(pw_hash))) {
		return -1;
	} else if (strptime(dob, "%m/%d/%Y", &birthday) == NULL || invalid_birthday(&birthday)) { /* Don't use %D since uses 2-digit years */
		bbs_debug(3, "Rejecting '%s' due to invalid DOB\n", dob);
		return -1; /* Invalid date */
	}

	/* We expect that the users table has a UNIQUE constraint on the username column
	 * Columns like date_registered and priv should be set automatically on INSERT. */
	snprintf(sql, sizeof(sql), "INSERT INTO %s%susers (username, password, name, email, phone, address, city, state, zip, dob, gender) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", DB_NAME_ARGS);

	mysql = sql_connect();
	NULL_RETURN(mysql);
	stmt = mysql_stmt_init(mysql);
	if (!stmt || sql_prep_bind_exec(stmt, sql, types, username, pw_hash, fullname, email, phone, address, city, state, zip, &birthday, genderbuf)) { /* Bind parameters and execute */
		goto cleanup;
	}
	res = 0;

cleanup:
	if (stmt) {
		mysql_stmt_close(stmt);
	}
	mysql_close(mysql);
	return res;
}

static int user_register(struct bbs_node *node)
{
	/* bcrypt caps password lengths at 72, so that's where that came from */
	char fullname[64], username[64], password[72], password2[72];
	char email[64], phone[16], address[64], city[64], state[32], zip[10], dob[11];
	char how_heard[256];
	char gender, correct;
	int res;
#define MAX_REG_ATTEMPTS 6
	int tries = MAX_REG_ATTEMPTS;

	bbs_buffer(node); /* Buffer input so we can read line by line */

#define REG_FMT COLOR(COLOR_WHITE)
#define REG_QLEN 43
#define get_response(node, qlen, fmt, q, pollms, buf, len, tries, minlen, reqchars) bbs_get_response(node, qlen, fmt q, pollms, buf, len, tries, minlen, reqchars)

	/* Registration notice */
	NEG_RETURN(bbs_clear_screen(node));
	NONPOS_RETURN(bbs_writef(node, "%s%s%s\n", COLOR(COLOR_PRIMARY), "New User Registration", COLOR(COLOR_WHITE))); /* Use white for the questions to stand out */

	for (; tries > 0; tries -= 2) {
		/* No newlines necessary inbetween reads, since echo is on
		 * and input is terminated by a return. */
		/* NONZERO_RETURN is a macro that returns x, so we must NOT call it directly with the function itself */
		res = get_response(node, REG_QLEN, REG_FMT, "How did you hear about this BBS? ", MIN_MS(1), how_heard, sizeof(how_heard), &tries, 0, NULL);
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "Please enter your full real name: ", MIN_MS(1), fullname, sizeof(fullname), &tries, 4, " "); /* If there's no space, we don't have at least 2 names */
		NONZERO_RETURN(res); 
		res = get_response(node, REG_QLEN, REG_FMT, "Desired username: ", MIN_MS(1), username, sizeof(username), &tries, 2, NULL);
		NONZERO_RETURN(res);

		bbs_echo_off(node); /* Don't display password */
		for (; tries > 0; tries--) { /* Retries here count less than retries of the main loop */
			NEG_RETURN(bbs_writef(node, "%-*s", REG_QLEN, REG_FMT "Password: "));
			NONPOS_RETURN(bbs_readline(node, MIN_MS(1), password, sizeof(password)));
			NEG_RETURN(bbs_writef(node, "%-*s", REG_QLEN, REG_FMT "\nConfirm Password: ")); /* Begin with new line since wasn't echoed */
			NONPOS_RETURN(bbs_readline(node, MIN_MS(1), password2, sizeof(password2)));
			if (s_strlen_zero(password) || strcmp(password, password2)) {
				NEG_RETURN(bbs_writef(node, "\n%sPasswords do not match%s\n", COLOR(COLOR_RED), COLOR_RESET));
			} else if (strlen(password) < 8) {
				NEG_RETURN(bbs_writef(node, "\n%sPassword is too short%s\n", COLOR(COLOR_RED), COLOR_RESET));
			} else {
				break;
			}
		}
		if (tries <= 0) {
			return 1;
		}
		bbs_echo_on(node);

		/* Begin with LF since not echoed from input */
		bbs_writef(node, "\n");

		/* XXX Validation of provided data needed, but should be primarily handled by the SQL schema. We do include some rudimentary format checks. */

		res = get_response(node, REG_QLEN, REG_FMT, "Network mail address (user@domain): ", MIN_MS(1), email, sizeof(email), &tries, 5, "@.");
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "Telephone Number: ", MIN_MS(1), phone, sizeof(phone), &tries, 7, NULL);
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "Street Address (Line 1/2): ", MIN_MS(1), address, sizeof(address), &tries, 6, " "); /* e.g. 1 E St */
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "City: ", MIN_MS(1), city, sizeof(city), &tries, 2, NULL);
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "State: ", MIN_MS(1), state, sizeof(state), &tries, 2, NULL);
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "ZIP/Postal Code: ", MIN_MS(1), zip, sizeof(zip), &tries, 3, NULL); /* US = 5, other countries??? */
		NONZERO_RETURN(res);
		res = get_response(node, REG_QLEN, REG_FMT, "Birthday (MM/DD/YYYY): ", MIN_MS(1), dob, sizeof(dob), &tries, 10, "/");
		NONZERO_RETURN(res);

		bbs_unbuffer(node); /* We need to be unbuffered for tread */
		for (; tries > 0; tries--) { /* Retries here count less than retries of the main loop */
			NEG_RETURN(bbs_writef(node, "%-*s", REG_QLEN, REG_FMT "\rGender (MFX): ")); /* Erase existing line in case we're retrying */
			gender = bbs_tread(node, MIN_MS(1));
			NONPOS_RETURN(gender);
			gender = tolower(gender);
			if (gender == 'm' || gender == 'f' || gender == 'x') {
				NEG_RETURN(bbs_writef(node, "%c\n", gender)); /* Print response + newline */
				break; /* Got a valid response */
			}
			/* Invalid, try again */
		}
		if (tries <= 0) {
			return 1;
		}

		NEG_RETURN(bbs_writef(node, "%-*s", REG_QLEN, REG_FMT "Is the above information correct? "));
		correct = bbs_tread(node, MIN_MS(1));
		if (tolower(correct) == 'y') {
			break;
		}
		/* Not correct? Start over! */
	}
	if (tries <= 0) {
		return 1;
	}
#undef REG_FMT

	NEG_RETURN(bbs_writef(node, "\n%sProcessing...\n", COLOR(COLOR_SUCCESS)));
	bbs_auth("New registration attempt for user %s from IP %s\n", username, node->ip);

	/* How heard is logged but not passed to make_user */
	bbs_debug(1, "New registration attempt: name = %s, username = %s, email = %s, phone = %s, address = %s, city = %s, state = %s, zip = %s, dob = %s, gender = %c, how heard = %s\n",
		fullname, username, email, phone, address, city, state, zip, dob, gender, how_heard);

	/* Actually create the user */
	res = make_user(username, password, fullname, email, phone, address, city, state, zip, dob, gender);

	if (res) {
		NEG_RETURN(bbs_writef(node, "%s%s%s\n", COLOR(COLOR_FAILURE), "Your registration was rejected.", COLOR_RESET));
		NEG_RETURN(bbs_wait_key(node, SEC_MS(75)));
		return 1;
	}
	/* If user registration actually succeeded, then this function call will succeed. If not, it won't. */
	res = bbs_authenticate(node, username, password);
	if (res) {
		/* Something went wrong */
		NEG_RETURN(bbs_writef(node, "%s%s%s\n", COLOR(COLOR_FAILURE), "An error occured in processing your registration.\n", COLOR_RESET));
		NEG_RETURN(bbs_wait_key(node, SEC_MS(75)));
		return 1;
	}

	/* If successful, no need to log, auth.c will do that */
	NEG_RETURN(bbs_writef(node, "\n%sRegistration successful. Welcome aboard!%s\n", COLOR(COLOR_SUCCESS), COLOR_RESET));
	/* Wait for user to confirm, otherwise the message will disappear since the screen will clear after we return */
	NEG_RETURN(bbs_wait_key(node, SEC_MS(75)));

	return res;
}

static int load_config(void)
{
	int res = 0;
	struct bbs_config *cfg = bbs_config_load("mod_auth_mysql.conf", 0);

	if (!cfg) {
		bbs_error("mod_auth_mysql.conf is missing, module will decline to load\n");
		return -1;
	}

	res |= bbs_config_val_set_str(cfg, "db", "hostname", buf_dbhostname, sizeof(buf_dbhostname));
	res |= bbs_config_val_set_str(cfg, "db", "username", buf_dbusername, sizeof(buf_dbusername));
	res |= bbs_config_val_set_str(cfg, "db", "password", buf_dbpassword, sizeof(buf_dbpassword));
	if (res) {
		bbs_error("Missing either hostname, username, or password\n");
		bbs_config_free(cfg);
		return -1;
	}
	if (bbs_config_val_set_str(cfg, "db", "database", buf_dbname, sizeof(buf_dbname))) { /* This is optional but highly recommended. */
		bbs_warning("No database name specified in mod_auth_mysql.conf\n");
	}

	bbs_config_free(cfg); /* Destroy the config now, rather than waiting until shutdown, since it will NEVER be used again for anything. */
	return 0;
}

static int load_module(void)
{
	if (load_config()) {
		return -1;
	}
	bbs_register_user_registration_provider(user_register);
	bbs_register_password_reset_handler(change_password);
	bbs_register_user_info_handler(get_user_info);
	return bbs_register_auth_provider("MySQL/MariaDB", provider);
}

static int unload_module(void)
{
	int res = bbs_unregister_auth_provider(provider);
	bbs_unregister_user_registration_provider(user_register);
	bbs_unregister_password_reset_handler(change_password);
	bbs_unregister_user_info_handler(get_user_info);
	mysql_library_end();
	return res;
}

BBS_MODULE_INFO_STANDARD("MySQL/MariaDB User Authentication");
