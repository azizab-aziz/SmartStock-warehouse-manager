#ifndef WMS_DB_H
#define WMS_DB_H

#include <sqlite3.h>
#include <stdbool.h>

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

#endif
