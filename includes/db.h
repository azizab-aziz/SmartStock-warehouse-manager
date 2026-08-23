#ifndef WMS_DB_H
#define WMS_DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Thin wrapper around a single sqlite3* connection.
 * We deliberately use ONE connection for all writes (SQLite serializes
 * writers per-database-file anyway) opened with WAL + busy_timeout, so
 * every write goes through db_begin_immediate()/db_commit()/db_rollback().
 */
typedef struct {
    sqlite3 *handle;
    char     path[512];
} WmsDb;

/* Opens (creating if needed) the database at `path`, applies schema.sql
 * on first run, sets WAL mode + busy_timeout + foreign_keys ON. */
bool db_open(WmsDb *db, const char *path);
void db_close(WmsDb *db);

/* Runs schema.sql (idempotent, CREATE TABLE IF NOT EXISTS everywhere). */
bool db_apply_schema(WmsDb *db, const char *schema_sql_path);

/*
 * Starts a write transaction with BEGIN IMMEDIATE, which acquires the
 * RESERVED lock right away instead of waiting for the first write
 * statement. This is what actually prevents the "two users, same second"
 * race: the second caller blocks here (or gets SQLITE_BUSY, retried by
 * db_begin_immediate_retry) instead of interleaving with the first.
 */
bool db_begin_immediate(WmsDb *db);
bool db_begin_immediate_retry(WmsDb *db, int max_retries);
bool db_commit(WmsDb *db);
bool db_rollback(WmsDb *db);


/* --- Authentication / session support --- */
bool db_verify_credentials(WmsDb *db, const char *username,
                            const char *password, int *out_user_id,
                            char *out_role, size_t role_len);
bool db_create_user(WmsDb *db, const char *username, const char *password,
                     const char *role, char *err_out, size_t err_len);
bool db_log_login(WmsDb *db, int user_id, int *out_login_log_id);
bool db_log_logout(WmsDb *db, int login_log_id);
bool db_count_users(WmsDb *db, int *out_count);

typedef struct {
    int  id;
    char name[64];
} Category;

int  db_list_categories(WmsDb *db, Category *out, int max_count);
bool db_find_or_create_category(WmsDb *db, const char *name, int *out_id);

int db_count_products_in_category(WmsDb *db, int category_id);

int  db_list_users(WmsDb *db, int *out_ids, char names[][64], char roles[][16], int max_count);
bool db_update_user_role(WmsDb *db, int user_id, const char *new_role);

#endif
