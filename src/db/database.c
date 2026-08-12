#define _DEFAULT_SOURCE
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool db_open(WmsDb *db, const char *path) {
    memset(db, 0, sizeof(*db));
    strncpy(db->path, path, sizeof(db->path) - 1);

    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(db->handle));
        return false;
    }

    /* busy_timeout tells SQLite's own C API to internally retry/wait up to
     * N ms before returning SQLITE_BUSY - our own retry loop below is a
     * second layer on top of this for the BEGIN IMMEDIATE step itself. */
    sqlite3_busy_timeout(db->handle, 3000);

    char *err = NULL;
    sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
    if (err) { fprintf(stderr, "WAL: %s\n", err); sqlite3_free(err); }

    sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (err) { fprintf(stderr, "FK: %s\n", err); sqlite3_free(err); }

    return true;
}

void db_close(WmsDb *db) {
    if (db->handle) {
        sqlite3_close(db->handle);
        db->handle = NULL;
    }
}

bool db_apply_schema(WmsDb *db, const char *schema_sql_path) {
    FILE *f = fopen(schema_sql_path, "rb");
    if (!f) {
        fprintf(stderr, "db_apply_schema: cannot open %s\n", schema_sql_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *sql = malloc((size_t)len + 1);
    if (!sql) { fclose(f); return false; }
    size_t read_n = fread(sql, 1, (size_t)len, f);
    sql[read_n] = '\0';
    fclose(f);

    char *err = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
    free(sql);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_apply_schema: %s\n", err ? err : "unknown error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool db_begin_immediate(WmsDb *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, "BEGIN IMMEDIATE;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false; /* likely SQLITE_BUSY: another writer holds the lock */
    }
    return true;
}

bool db_begin_immediate_retry(WmsDb *db, int max_retries) {
    int delay_ms = 20;
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (db_begin_immediate(db)) return true;
        usleep((useconds_t)delay_ms * 1000);
        delay_ms = (delay_ms * 2 > 500) ? 500 : delay_ms * 2; /* backoff, cap 500ms */
    }
    return false;
}

bool db_commit(WmsDb *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, "COMMIT;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_commit: %s\n", err ? err : "unknown error");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool db_rollback(WmsDb *db) {
    sqlite3_exec(db->handle, "ROLLBACK;", NULL, NULL, NULL);
    return true;
}
