#define _DEFAULT_SOURCE
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sodium.h>

bool db_open(WmsDb *db, const char *path) {
    if (sodium_init() < 0) {
        fprintf(stderr, "db_open: sodium_init failed\n");
        return false;
    }
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

    /* Migration: existing installs already have a `products` table without
       category_id. SQLite has no "ADD COLUMN IF NOT EXISTS" -- we just try
       it and ignore the "duplicate column name" error if it's already there
       (same pattern as the users/locations schema-drift issue). */
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN category_id INTEGER REFERENCES categories(id);",
        NULL, NULL, NULL);

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

bool db_verify_credentials(WmsDb *db, const char *username, const char *password,
                            int *out_user_id, char *out_role, size_t role_len) {
    const char *sql =
        "SELECT id, password_hash, role FROM users "
        "WHERE username = ?1 AND active = 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *hash = sqlite3_column_text(stmt, 1);
        const unsigned char *role = sqlite3_column_text(stmt, 2);

        if (hash && crypto_pwhash_str_verify((const char *)hash, password,
                                              strlen(password)) == 0) {
            *out_user_id = id;
            snprintf(out_role, role_len, "%s", role ? (const char *)role : "");
            ok = true;
        }
    }
    sqlite3_finalize(stmt);

    if (ok) {
        sqlite3_stmt *upd;
        sqlite3_prepare_v2(db->handle,
            "UPDATE users SET last_login_at = datetime('now') WHERE id = ?1;",
            -1, &upd, NULL);
        sqlite3_bind_int(upd, 1, *out_user_id);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
    }
    return ok;
}

bool db_create_user(WmsDb *db, const char *username, const char *password,
                     const char *role, char *err_out, size_t err_len) {
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password, strlen(password),
                           crypto_pwhash_OPSLIMIT_INTERACTIVE,
                           crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        snprintf(err_out, err_len, "Erreur de hachage du mot de passe (mémoire insuffisante)");
        return false;
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO users (username, password_hash, role) VALUES (?1, ?2, ?3);",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, role, -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return ok;
}

bool db_log_login(WmsDb *db, int user_id, int *out_login_log_id) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO login_log (user_id) VALUES (?1);", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) *out_login_log_id = (int)sqlite3_last_insert_rowid(db->handle);
    return ok;
}

bool db_log_logout(WmsDb *db, int login_log_id) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db->handle,
        "UPDATE login_log SET logout_at = datetime('now'), "
        "duration_s = CAST((julianday('now') - julianday(login_at)) * 86400 AS INTEGER) "
        "WHERE id = ?1;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, login_log_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool db_count_users(WmsDb *db, int *out_count) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM users;", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out_count = sqlite3_column_int(stmt, 0);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

int db_list_categories(WmsDb *db, Category *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id, name FROM categories ORDER BY name COLLATE NOCASE;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = sqlite3_column_int(st, 0);
        snprintf(out[n].name, sizeof(out[n].name), "%s", (const char*)sqlite3_column_text(st, 1));
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

bool db_find_or_create_category(WmsDb *db, const char *name, int *out_id) {
    char trimmed[64];
    const char *start = name;
    while (*start == ' ') start++;
    snprintf(trimmed, sizeof(trimmed), "%s", start);
    int len = (int)strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == ' ') trimmed[--len] = '\0';
    if (trimmed[0] == '\0') return false;

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id FROM categories WHERE name = ?1 COLLATE NOCASE;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return true;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db->handle, "INSERT INTO categories (name) VALUES (?1);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) return false;
    *out_id = (int)sqlite3_last_insert_rowid(db->handle);
    return true;
}

int db_count_products_in_category(WmsDb *db, int category_id) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT COUNT(*) FROM products WHERE category_id = ?1;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, category_id);
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}
