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
    sqlite3_exec(db->handle,
        "ALTER TABLE products ADD COLUMN active INTEGER NOT NULL DEFAULT 1;",
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
