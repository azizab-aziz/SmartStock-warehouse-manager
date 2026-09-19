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

/* One-time backfill: reconstructs "creation" audit entries for rows that
 * already existed before this logging feature was added, using each
 * table's own created_at timestamp. Attributed to no user ("systeme"),
 * since the app never recorded who created these rows historically -
 * that information genuinely doesn't exist anywhere to recover. Only
 * runs once - guarded by checking audit_log is still empty, so it never
 * duplicates entries once real logging has taken over. Edits and
 * deletions from before this feature existed can NOT be reconstructed;
 * only initial creation is recoverable via created_at. */
static void db_backfill_audit_log(WmsDb *db) {
    int existing = 0;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM audit_log;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) existing = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (existing > 0) return; /* already backfilled, or real entries exist - never re-run */

    const char *note = "Action historique (anterieure a l'activation du journal - utilisateur inconnu)";

    sqlite3_prepare_v2(db->handle,
        "INSERT INTO audit_log (user_id, action, entity_type, entity_label, details, created_at) "
        "SELECT NULL, 'creation', 'produit', name, ?, created_at FROM products;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, note, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db->handle,
        "INSERT INTO audit_log (user_id, action, entity_type, entity_label, details, created_at) "
        "SELECT NULL, 'creation', 'fournisseur', name, ?, created_at FROM suppliers;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, note, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db->handle,
        "INSERT INTO audit_log (user_id, action, entity_type, entity_label, details, created_at) "
        "SELECT NULL, 'creation', 'utilisateur', username, ?, created_at FROM users;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, note, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db->handle,
        "INSERT INTO audit_log (user_id, action, entity_type, entity_label, details, created_at) "
        "SELECT NULL, 'creation', 'commande', po_number, ?, created_at FROM purchase_orders;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, note, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
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
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN active INTEGER NOT NULL DEFAULT 1;",
        NULL, NULL, NULL);
    /* Migration: brand/description/status for existing installs created
       before these columns existed. Same "try and ignore duplicate column"
       pattern as category_id/active above. ALTER TABLE ADD COLUMN cannot
       carry the CHECK over for status - the constraint already lives in
       schema.sql for fresh installs, and writes are validated by only
       ever sending the three known enum strings (see GUI/status code). */
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN brand TEXT;", NULL, NULL, NULL);
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN description TEXT;", NULL, NULL, NULL);
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN status TEXT NOT NULL DEFAULT 'actif';",
        NULL, NULL, NULL);

    /* New tables for supplier info + purchase/receiving records.
       CREATE TABLE IF NOT EXISTS is idempotent, safe to run every startup -
       same reasoning as the ALTER TABLE calls above, just for brand-new
       tables instead of existing ones. */
    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS suppliers ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  contact_name TEXT,"
        "  phone TEXT,"
        "  email TEXT,"
        "  address TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS purchase_orders ("
        "  id INTEGER PRIMARY KEY,"
        "  supplier_id INTEGER REFERENCES suppliers(id),"
        "  po_number TEXT UNIQUE NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'brouillon',"
        "  reference TEXT,"
        "  created_by INTEGER REFERENCES users(id),"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  received_at TEXT"
        ");", NULL, NULL, NULL);

            sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS purchase_order_items ("
        "  id INTEGER PRIMARY KEY,"
        "  po_id INTEGER NOT NULL REFERENCES purchase_orders(id),"
        "  product_id INTEGER REFERENCES products(id),"
        "  quantity_ordered INTEGER NOT NULL,"
        "  quantity_received INTEGER NOT NULL DEFAULT 0,"
        "  unit_cost REAL NOT NULL DEFAULT 0"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS customers ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  contact_name TEXT,"
        "  phone TEXT,"
        "  email TEXT,"
        "  address TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS dispatch_orders ("
        "  id INTEGER PRIMARY KEY,"
        "  customer_id INTEGER REFERENCES customers(id),"
        "  do_number TEXT UNIQUE NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'brouillon',"
        "  reference TEXT,"
        "  created_by INTEGER REFERENCES users(id),"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  shipped_at TEXT"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS dispatch_order_items ("
        "  id INTEGER PRIMARY KEY,"
        "  do_id INTEGER NOT NULL REFERENCES dispatch_orders(id),"
        "  product_id INTEGER REFERENCES products(id),"
        "  quantity_ordered INTEGER NOT NULL,"
        "  quantity_shipped INTEGER NOT NULL DEFAULT 0,"
        "  unit_price REAL NOT NULL DEFAULT 0"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS returns ("
        "  id INTEGER PRIMARY KEY,"
        "  do_id INTEGER REFERENCES dispatch_orders(id),"
        "  product_id INTEGER REFERENCES products(id),"
        "  quantity INTEGER NOT NULL,"
        "  reason TEXT,"
        "  processed_by INTEGER REFERENCES users(id),"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");", NULL, NULL, NULL);
    sqlite3_exec(db->handle,
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "  id INTEGER PRIMARY KEY,"
        "  user_id INTEGER REFERENCES users(id),"
        "  action TEXT NOT NULL,"
        "  entity_type TEXT NOT NULL,"
        "  entity_label TEXT NOT NULL,"
        "  details TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");", NULL, NULL, NULL);

       db_backfill_audit_log(db);

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
                "SELECT COUNT(*) FROM products WHERE category_id = ?1 AND active = 1;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, category_id);
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

int db_list_users(WmsDb *db, int *out_ids, char names[][64], char roles[][16], int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id, username, role FROM users ORDER BY username;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        out_ids[n] = sqlite3_column_int(st, 0);
        snprintf(names[n], 64, "%s", (const char*)sqlite3_column_text(st, 1));
        snprintf(roles[n], 16, "%s", (const char*)sqlite3_column_text(st, 2));
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

bool db_update_user_role(WmsDb *db, int user_id, const char *new_role) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE users SET role = ?1 WHERE id = ?2;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, new_role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, user_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool db_update_category_name(WmsDb *db, int category_id, const char *new_name) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE categories SET name = ?1 WHERE id = ?2;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, new_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, category_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

int db_list_suppliers(WmsDb *db, Supplier *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id, name, COALESCE(contact_name,''), COALESCE(phone,''), "
        "COALESCE(email,''), COALESCE(address,'') FROM suppliers ORDER BY name COLLATE NOCASE;",
        -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = sqlite3_column_int(st, 0);
        snprintf(out[n].name, sizeof out[n].name, "%s", (const char*)sqlite3_column_text(st, 1));
        snprintf(out[n].contact_name, sizeof out[n].contact_name, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(out[n].phone, sizeof out[n].phone, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(out[n].email, sizeof out[n].email, "%s", (const char*)sqlite3_column_text(st, 4));
        snprintf(out[n].address, sizeof out[n].address, "%s", (const char*)sqlite3_column_text(st, 5));
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

bool db_create_supplier(WmsDb *db, const Supplier *s, char *err_out, size_t err_len) {
    if (!s->name[0]) { snprintf(err_out, err_len, "Le nom du fournisseur est obligatoire"); return false; }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO suppliers (name, contact_name, phone, email, address) VALUES (?,?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, s->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s->contact_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, s->address, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_update_supplier(WmsDb *db, const Supplier *s, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE suppliers SET name=?, contact_name=?, phone=?, email=?, address=? WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, s->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s->contact_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, s->address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, s->id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_delete_supplier(WmsDb *db, int supplier_id, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT COUNT(*) FROM products WHERE supplier_id=? AND active=1;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, supplier_id);
    int in_use = 0;
    if (sqlite3_step(st) == SQLITE_ROW) in_use = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (in_use > 0) {
        snprintf(err_out, err_len, "Impossible: %d produit(s) utilisent encore ce fournisseur", in_use);
        return false;
    }
    sqlite3_prepare_v2(db->handle, "DELETE FROM suppliers WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, supplier_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_create_purchase_order(WmsDb *db, int supplier_id, int created_by,
                               const char *reference, int *out_po_id,
                               char *err_out, size_t err_len) {
    int next_num = 1;
    sqlite3_stmt *num_st;
    sqlite3_prepare_v2(db->handle,
        "SELECT COALESCE(MAX(CAST(SUBSTR(po_number,4) AS INTEGER)),0)+1 FROM purchase_orders;",
        -1, &num_st, NULL);
    if (sqlite3_step(num_st) == SQLITE_ROW) next_num = sqlite3_column_int(num_st, 0);
    sqlite3_finalize(num_st);

    char po_number[32];
    snprintf(po_number, sizeof po_number, "PO-%04d", next_num);

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO purchase_orders (supplier_id, po_number, status, reference, created_by) "
        "VALUES (?,?,'brouillon',?,?);", -1, &st, NULL);
    if (supplier_id > 0) sqlite3_bind_int(st, 1, supplier_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, po_number, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, reference ? reference : "", -1, SQLITE_TRANSIENT);
    if (created_by > 0) sqlite3_bind_int(st, 4, created_by); else sqlite3_bind_null(st, 4);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
        sqlite3_finalize(st);
        return false;
    }
    *out_po_id = (int)sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(st);
    return true;
}

bool db_add_po_item(WmsDb *db, int po_id, int product_id, int quantity_ordered,
                    double unit_cost, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO purchase_order_items (po_id, product_id, quantity_ordered, unit_cost) "
        "VALUES (?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_int(st, 1, po_id);
    sqlite3_bind_int(st, 2, product_id);
    sqlite3_bind_int(st, 3, quantity_ordered);
    sqlite3_bind_double(st, 4, unit_cost);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

int db_list_purchase_orders(WmsDb *db, PurchaseOrder *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT po.id, COALESCE(po.supplier_id,0), COALESCE(s.name,'Sans fournisseur'), "
        "po.po_number, po.status, COALESCE(po.reference,''), COALESCE(po.created_by,0), "
        "COALESCE(u.username,''), po.created_at, COALESCE(po.received_at,'') "
        "FROM purchase_orders po "
        "LEFT JOIN suppliers s ON s.id = po.supplier_id "
        "LEFT JOIN users u ON u.id = po.created_by "
        "ORDER BY po.created_at DESC, po.id DESC;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        PurchaseOrder *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->id = sqlite3_column_int(st, 0);
        p->supplier_id = sqlite3_column_int(st, 1);
        snprintf(p->supplier_name, sizeof p->supplier_name, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(p->po_number, sizeof p->po_number, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(p->status, sizeof p->status, "%s", (const char*)sqlite3_column_text(st, 4));
        snprintf(p->reference, sizeof p->reference, "%s", (const char*)sqlite3_column_text(st, 5));
        p->created_by = sqlite3_column_int(st, 6);
        snprintf(p->created_by_name, sizeof p->created_by_name, "%s", (const char*)sqlite3_column_text(st, 7));
        snprintf(p->created_at, sizeof p->created_at, "%s", (const char*)sqlite3_column_text(st, 8));
        snprintf(p->received_at, sizeof p->received_at, "%s", (const char*)sqlite3_column_text(st, 9));
    }
    sqlite3_finalize(st);
    return n;
}

int db_get_po_items(WmsDb *db, int po_id, PurchaseOrderItem *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT poi.id, poi.po_id, poi.product_id, COALESCE(p.name,'Produit supprime'), "
        "COALESCE(p.sku,''), poi.quantity_ordered, poi.quantity_received, poi.unit_cost "
        "FROM purchase_order_items poi LEFT JOIN products p ON p.id = poi.product_id "
        "WHERE poi.po_id = ?1 ORDER BY poi.id;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, po_id);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        PurchaseOrderItem *it = &out[n++];
        memset(it, 0, sizeof(*it));
        it->id = sqlite3_column_int(st, 0);
        it->po_id = sqlite3_column_int(st, 1);
        it->product_id = sqlite3_column_int(st, 2);
        snprintf(it->product_name, sizeof it->product_name, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(it->product_sku, sizeof it->product_sku, "%s", (const char*)sqlite3_column_text(st, 4));
        it->quantity_ordered = sqlite3_column_int(st, 5);
        it->quantity_received = sqlite3_column_int(st, 6);
        it->unit_cost = sqlite3_column_double(st, 7);
    }
    sqlite3_finalize(st);
    return n;
}

bool db_update_po_item_received(WmsDb *db, int po_item_id, int new_received_qty,
                                 char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE purchase_order_items SET quantity_received=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, new_received_qty);
    sqlite3_bind_int(st, 2, po_item_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_update_po_status(WmsDb *db, int po_id, const char *new_status) {
    sqlite3_stmt *st;
    if (strcmp(new_status, "recu") == 0) {
        sqlite3_prepare_v2(db->handle,
            "UPDATE purchase_orders SET status=?, received_at=datetime('now') WHERE id=?;", -1, &st, NULL);
    } else {
        sqlite3_prepare_v2(db->handle,
            "UPDATE purchase_orders SET status=? WHERE id=?;", -1, &st, NULL);
    }
    sqlite3_bind_text(st, 1, new_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, po_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

int db_list_locations(WmsDb *db, Location *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id, code, aisle, shelf, bin, capacity FROM locations ORDER BY code COLLATE NOCASE;",
        -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = sqlite3_column_int(st, 0);
        snprintf(out[n].code, sizeof out[n].code, "%s", (const char*)sqlite3_column_text(st, 1));
        snprintf(out[n].aisle, sizeof out[n].aisle, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(out[n].shelf, sizeof out[n].shelf, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(out[n].bin, sizeof out[n].bin, "%s", (const char*)sqlite3_column_text(st, 4));
        out[n].capacity = sqlite3_column_int(st, 5);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

bool db_create_location(WmsDb *db, const char *code, const char *aisle, const char *shelf,
                         const char *bin, int capacity, char *err_out, size_t err_len) {
    if (!code[0] || !aisle[0] || !shelf[0] || !bin[0]) {
        snprintf(err_out, err_len, "Code, allee, etagere et bac sont obligatoires");
        return false;
    }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO locations (code, aisle, shelf, bin, capacity) VALUES (?,?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, code, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, aisle, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, shelf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, bin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, capacity);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle)); /* e.g. UNIQUE constraint on code */
    sqlite3_finalize(st);
    return ok;
}

bool db_log_audit(WmsDb *db, int user_id, const char *action, const char *entity_type,
                   const char *entity_label, const char *details) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO audit_log (user_id, action, entity_type, entity_label, details) "
        "VALUES (?,?,?,?,?);", -1, &st, NULL);
    if (user_id > 0) sqlite3_bind_int(st, 1, user_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, entity_label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, details ? details : "", -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

int db_list_audit_log(WmsDb *db, AuditLogEntry *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT a.id, COALESCE(a.user_id,0), COALESCE(u.username,'systeme'), a.action, "
        "a.entity_type, a.entity_label, COALESCE(a.details,''), a.created_at "
        "FROM audit_log a LEFT JOIN users u ON u.id = a.user_id "
        "ORDER BY a.created_at DESC, a.id DESC;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        AuditLogEntry *e = &out[n++];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int(st, 0);
        e->user_id = sqlite3_column_int(st, 1);
        snprintf(e->username, sizeof e->username, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(e->action, sizeof e->action, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(e->entity_type, sizeof e->entity_type, "%s", (const char*)sqlite3_column_text(st, 4));
        snprintf(e->entity_label, sizeof e->entity_label, "%s", (const char*)sqlite3_column_text(st, 5));
        snprintf(e->details, sizeof e->details, "%s", (const char*)sqlite3_column_text(st, 6));
        snprintf(e->created_at, sizeof e->created_at, "%s", (const char*)sqlite3_column_text(st, 7));
    }
    sqlite3_finalize(st);
    return n;
}

int db_list_customers(WmsDb *db, Customer *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT id, name, COALESCE(contact_name,''), COALESCE(phone,''), "
        "COALESCE(email,''), COALESCE(address,'') FROM customers ORDER BY name COLLATE NOCASE;",
        -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = sqlite3_column_int(st, 0);
        snprintf(out[n].name, sizeof out[n].name, "%s", (const char*)sqlite3_column_text(st, 1));
        snprintf(out[n].contact_name, sizeof out[n].contact_name, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(out[n].phone, sizeof out[n].phone, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(out[n].email, sizeof out[n].email, "%s", (const char*)sqlite3_column_text(st, 4));
        snprintf(out[n].address, sizeof out[n].address, "%s", (const char*)sqlite3_column_text(st, 5));
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

bool db_create_customer(WmsDb *db, const Customer *c, char *err_out, size_t err_len) {
    if (!c->name[0]) { snprintf(err_out, err_len, "Le nom du client est obligatoire"); return false; }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO customers (name, contact_name, phone, email, address) VALUES (?,?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, c->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->contact_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, c->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, c->address, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_update_customer(WmsDb *db, const Customer *c, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE customers SET name=?, contact_name=?, phone=?, email=?, address=? WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, c->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c->contact_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, c->phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, c->email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, c->address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, c->id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_delete_customer(WmsDb *db, int customer_id, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT COUNT(*) FROM dispatch_orders WHERE customer_id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, customer_id);
    int in_use = 0;
    if (sqlite3_step(st) == SQLITE_ROW) in_use = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (in_use > 0) {
        snprintf(err_out, err_len, "Impossible: %d commande(s) client existent deja pour ce client", in_use);
        return false;
    }
    sqlite3_prepare_v2(db->handle, "DELETE FROM customers WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, customer_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_create_dispatch_order(WmsDb *db, int customer_id, int created_by,
                               const char *reference, int *out_do_id,
                               char *err_out, size_t err_len) {
    int next_num = 1;
    sqlite3_stmt *num_st;
    sqlite3_prepare_v2(db->handle,
        "SELECT COALESCE(MAX(CAST(SUBSTR(do_number,4) AS INTEGER)),0)+1 FROM dispatch_orders;",
        -1, &num_st, NULL);
    if (sqlite3_step(num_st) == SQLITE_ROW) next_num = sqlite3_column_int(num_st, 0);
    sqlite3_finalize(num_st);

    char do_number[32];
    snprintf(do_number, sizeof do_number, "BL-%04d", next_num);

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO dispatch_orders (customer_id, do_number, status, reference, created_by) "
        "VALUES (?,?,'brouillon',?,?);", -1, &st, NULL);
    if (customer_id > 0) sqlite3_bind_int(st, 1, customer_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, do_number, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, reference ? reference : "", -1, SQLITE_TRANSIENT);
    if (created_by > 0) sqlite3_bind_int(st, 4, created_by); else sqlite3_bind_null(st, 4);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
        sqlite3_finalize(st);
        return false;
    }
    *out_do_id = (int)sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(st);
    return true;
}

bool db_add_do_item(WmsDb *db, int do_id, int product_id, int quantity_ordered,
                    double unit_price, char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO dispatch_order_items (do_id, product_id, quantity_ordered, unit_price) "
        "VALUES (?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_int(st, 1, do_id);
    sqlite3_bind_int(st, 2, product_id);
    sqlite3_bind_int(st, 3, quantity_ordered);
    sqlite3_bind_double(st, 4, unit_price);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

int db_list_dispatch_orders(WmsDb *db, DispatchOrder *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT d.id, COALESCE(d.customer_id,0), COALESCE(c.name,'Sans client'), "
        "d.do_number, d.status, COALESCE(d.reference,''), COALESCE(d.created_by,0), "
        "COALESCE(u.username,''), d.created_at, COALESCE(d.shipped_at,'') "
        "FROM dispatch_orders d "
        "LEFT JOIN customers c ON c.id = d.customer_id "
        "LEFT JOIN users u ON u.id = d.created_by "
        "ORDER BY d.created_at DESC, d.id DESC;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        DispatchOrder *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->id = sqlite3_column_int(st, 0);
        p->customer_id = sqlite3_column_int(st, 1);
        snprintf(p->customer_name, sizeof p->customer_name, "%s", (const char*)sqlite3_column_text(st, 2));
        snprintf(p->do_number, sizeof p->do_number, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(p->status, sizeof p->status, "%s", (const char*)sqlite3_column_text(st, 4));
        snprintf(p->reference, sizeof p->reference, "%s", (const char*)sqlite3_column_text(st, 5));
        p->created_by = sqlite3_column_int(st, 6);
        snprintf(p->created_by_name, sizeof p->created_by_name, "%s", (const char*)sqlite3_column_text(st, 7));
        snprintf(p->created_at, sizeof p->created_at, "%s", (const char*)sqlite3_column_text(st, 8));
        snprintf(p->shipped_at, sizeof p->shipped_at, "%s", (const char*)sqlite3_column_text(st, 9));
    }
    sqlite3_finalize(st);
    return n;
}

int db_get_do_items(WmsDb *db, int do_id, DispatchOrderItem *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT doi.id, doi.do_id, doi.product_id, COALESCE(p.name,'Produit supprime'), "
        "COALESCE(p.sku,''), doi.quantity_ordered, doi.quantity_shipped, doi.unit_price "
        "FROM dispatch_order_items doi LEFT JOIN products p ON p.id = doi.product_id "
        "WHERE doi.do_id = ?1 ORDER BY doi.id;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, do_id);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        DispatchOrderItem *it = &out[n++];
        memset(it, 0, sizeof(*it));
        it->id = sqlite3_column_int(st, 0);
        it->do_id = sqlite3_column_int(st, 1);
        it->product_id = sqlite3_column_int(st, 2);
        snprintf(it->product_name, sizeof it->product_name, "%s", (const char*)sqlite3_column_text(st, 3));
        snprintf(it->product_sku, sizeof it->product_sku, "%s", (const char*)sqlite3_column_text(st, 4));
        it->quantity_ordered = sqlite3_column_int(st, 5);
        it->quantity_shipped = sqlite3_column_int(st, 6);
        it->unit_price = sqlite3_column_double(st, 7);
    }
    sqlite3_finalize(st);
    return n;
}

bool db_update_do_item_shipped(WmsDb *db, int do_item_id, int new_shipped_qty,
                                 char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "UPDATE dispatch_order_items SET quantity_shipped=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, new_shipped_qty);
    sqlite3_bind_int(st, 2, do_item_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(st);
    return ok;
}

bool db_update_do_status(WmsDb *db, int do_id, const char *new_status) {
    sqlite3_stmt *st;
    if (strcmp(new_status, "expedie") == 0) {
        sqlite3_prepare_v2(db->handle,
            "UPDATE dispatch_orders SET status=?, shipped_at=datetime('now') WHERE id=?;", -1, &st, NULL);
    } else {
        sqlite3_prepare_v2(db->handle,
            "UPDATE dispatch_orders SET status=? WHERE id=?;", -1, &st, NULL);
    }
    sqlite3_bind_text(st, 1, new_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, do_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool db_create_return(WmsDb *db, int do_id, int product_id, int quantity,
                       const char *reason, int processed_by, int *out_return_id,
                       char *err_out, size_t err_len) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "INSERT INTO returns (do_id, product_id, quantity, reason, processed_by) "
        "VALUES (?,?,?,?,?);", -1, &st, NULL);
    if (do_id > 0) sqlite3_bind_int(st, 1, do_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_int(st, 2, product_id);
    sqlite3_bind_int(st, 3, quantity);
    sqlite3_bind_text(st, 4, reason ? reason : "", -1, SQLITE_TRANSIENT);
    if (processed_by > 0) sqlite3_bind_int(st, 5, processed_by); else sqlite3_bind_null(st, 5);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        snprintf(err_out, err_len, "%s", sqlite3_errmsg(db->handle));
        sqlite3_finalize(st);
        return false;
    }
    *out_return_id = (int)sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(st);
    return true;
}

int db_list_returns(WmsDb *db, ReturnRecord *out, int max_count) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db->handle,
        "SELECT r.id, COALESCE(r.do_id,0), COALESCE(d.do_number,''), r.product_id, "
        "COALESCE(p.name,'Produit supprime'), r.quantity, COALESCE(r.reason,''), "
        "COALESCE(r.processed_by,0), COALESCE(u.username,''), r.created_at "
        "FROM returns r "
        "LEFT JOIN dispatch_orders d ON d.id = r.do_id "
        "LEFT JOIN products p ON p.id = r.product_id "
        "LEFT JOIN users u ON u.id = r.processed_by "
        "ORDER BY r.created_at DESC, r.id DESC;", -1, &st, NULL);
    int n = 0;
    while (n < max_count && sqlite3_step(st) == SQLITE_ROW) {
        ReturnRecord *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int(st, 0);
        r->do_id = sqlite3_column_int(st, 1);
        snprintf(r->do_number, sizeof r->do_number, "%s", (const char*)sqlite3_column_text(st, 2));
        r->product_id = sqlite3_column_int(st, 3);
        snprintf(r->product_name, sizeof r->product_name, "%s", (const char*)sqlite3_column_text(st, 4));
        r->quantity = sqlite3_column_int(st, 5);
        snprintf(r->reason, sizeof r->reason, "%s", (const char*)sqlite3_column_text(st, 6));
        r->processed_by = sqlite3_column_int(st, 7);
        snprintf(r->processed_by_name, sizeof r->processed_by_name, "%s", (const char*)sqlite3_column_text(st, 8));
        snprintf(r->created_at, sizeof r->created_at, "%s", (const char*)sqlite3_column_text(st, 9));
    }
    sqlite3_finalize(st);
    return n;
}

/* Shared with inventory.c's csv_write_field via a local copy - database.c
 * has no dependency on inventory.c, so this is a small local duplicate
 * rather than a cross-module include just for one helper. */
static void db_csv_write_field(FILE *f, const char *text) {
    bool needs_quotes = (strchr(text, ',') != NULL) || (strchr(text, '"') != NULL);
    if (!needs_quotes) { fputs(text, f); return; }
    fputc('"', f);
    for (const char *c = text; *c; c++) {
        if (*c == '"') fputc('"', f);
        fputc(*c, f);
    }
    fputc('"', f);
}

static FILE *db_csv_open(const char *path, char *err_out, size_t err_len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err_out, err_len, "Impossible de creer le fichier CSV (dossier manquant ?)");
        return NULL;
    }
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f); /* UTF-8 BOM */
    return f;
}

bool db_export_suppliers_csv(WmsDb *db, const char *path, char *err_out, size_t err_len) {
    FILE *f = db_csv_open(path, err_out, err_len);
    if (!f) return false;
    fprintf(f, "Nom,Contact,Telephone,Email,Adresse\r\n");

    Supplier suppliers[256];
    int count = db_list_suppliers(db, suppliers, 256);
    for (int i = 0; i < count; i++) {
        Supplier *s = &suppliers[i];
        db_csv_write_field(f, s->name); fputc(',', f);
        db_csv_write_field(f, s->contact_name); fputc(',', f);
        db_csv_write_field(f, s->phone); fputc(',', f);
        db_csv_write_field(f, s->email); fputc(',', f);
        db_csv_write_field(f, s->address);
        fprintf(f, "\r\n");
    }
    fclose(f);
    return true;
}

bool db_export_customers_csv(WmsDb *db, const char *path, char *err_out, size_t err_len) {
    FILE *f = db_csv_open(path, err_out, err_len);
    if (!f) return false;
    fprintf(f, "Nom,Contact,Telephone,Email,Adresse\r\n");

    Customer customers[256];
    int count = db_list_customers(db, customers, 256);
    for (int i = 0; i < count; i++) {
        Customer *c = &customers[i];
        db_csv_write_field(f, c->name); fputc(',', f);
        db_csv_write_field(f, c->contact_name); fputc(',', f);
        db_csv_write_field(f, c->phone); fputc(',', f);
        db_csv_write_field(f, c->email); fputc(',', f);
        db_csv_write_field(f, c->address);
        fprintf(f, "\r\n");
    }
    fclose(f);
    return true;
}

bool db_export_po_items_csv(WmsDb *db, const char *path, char *err_out, size_t err_len) {
    FILE *f = db_csv_open(path, err_out, err_len);
    if (!f) return false;
    fprintf(f, "PO_Numero,Fournisseur,Statut,Date_Creation,SKU,Produit,Qte_Commandee,Qte_Recue,Cout_Unitaire\r\n");

    PurchaseOrder orders[256];
    int order_count = db_list_purchase_orders(db, orders, 256);
    for (int i = 0; i < order_count; i++) {
        PurchaseOrder *po = &orders[i];
        PurchaseOrderItem items[256];
        int item_count = db_get_po_items(db, po->id, items, 256);
        for (int j = 0; j < item_count; j++) {
            PurchaseOrderItem *it = &items[j];
            db_csv_write_field(f, po->po_number); fputc(',', f);
            db_csv_write_field(f, po->supplier_name); fputc(',', f);
            db_csv_write_field(f, po->status); fputc(',', f);
            db_csv_write_field(f, po->created_at); fputc(',', f);
            db_csv_write_field(f, it->product_sku); fputc(',', f);
            db_csv_write_field(f, it->product_name); fputc(',', f);
            fprintf(f, "%d,%d,%.2f\r\n", it->quantity_ordered, it->quantity_received, it->unit_cost);
        }
    }
    fclose(f);
    return true;
}

bool db_export_do_items_csv(WmsDb *db, const char *path, char *err_out, size_t err_len) {
    FILE *f = db_csv_open(path, err_out, err_len);
    if (!f) return false;
    fprintf(f, "BL_Numero,Client,Statut,Date_Creation,SKU,Produit,Qte_Commandee,Qte_Expediee,Prix_Unitaire\r\n");

    DispatchOrder orders[256];
    int order_count = db_list_dispatch_orders(db, orders, 256);
    for (int i = 0; i < order_count; i++) {
        DispatchOrder *do_ = &orders[i];
        DispatchOrderItem items[256];
        int item_count = db_get_do_items(db, do_->id, items, 256);
        for (int j = 0; j < item_count; j++) {
            DispatchOrderItem *it = &items[j];
            db_csv_write_field(f, do_->do_number); fputc(',', f);
            db_csv_write_field(f, do_->customer_name); fputc(',', f);
            db_csv_write_field(f, do_->status); fputc(',', f);
            db_csv_write_field(f, do_->created_at); fputc(',', f);
            db_csv_write_field(f, it->product_sku); fputc(',', f);
            db_csv_write_field(f, it->product_name); fputc(',', f);
            fprintf(f, "%d,%d,%.2f\r\n", it->quantity_ordered, it->quantity_shipped, it->unit_price);
        }
    }
    fclose(f);
    return true;
}

bool db_export_returns_csv(WmsDb *db, const char *path, char *err_out, size_t err_len) {
    FILE *f = db_csv_open(path, err_out, err_len);
    if (!f) return false;
    fprintf(f, "Date,Produit,Quantite,BL_Lie,Raison,Traite_Par\r\n");

    ReturnRecord returns[256];
    int count = db_list_returns(db, returns, 256);
    for (int i = 0; i < count; i++) {
        ReturnRecord *r = &returns[i];
        db_csv_write_field(f, r->created_at); fputc(',', f);
        db_csv_write_field(f, r->product_name); fputc(',', f);
        fprintf(f, "%d,", r->quantity);
        db_csv_write_field(f, r->do_number[0] ? r->do_number : "-"); fputc(',', f);
        db_csv_write_field(f, r->reason); fputc(',', f);
        db_csv_write_field(f, r->processed_by_name[0] ? r->processed_by_name : "-");
        fprintf(f, "\r\n");
    }
    fclose(f);
    return true;
}
