#include "core.h"
#include "hashtable.h"
#include "session.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static WmsDb     *g_db = NULL;
static Product    g_products[WMS_MAX_PRODUCTS];
static int         g_product_count = 0;
#define WMS_MAX_CATEGORIES 256
static Category g_categories[WMS_MAX_CATEGORIES];
static int       g_category_count = 0;
static HashTable   g_by_sku;
static HashTable   g_by_barcode;

static const char *movement_type_str(MovementType t) {
    switch (t) {
        case MV_RECEPTION:     return "reception";
        case MV_RETOUR:        return "retour";
        case MV_ADJUST_POS:    return "ajustement+";
        case MV_EXPEDITION:    return "expedition";
        case MV_PERTE:         return "perte";
        case MV_ADJUST_NEG:    return "ajustement-";
        case MV_TRANSFER_IN:   return "transfert_in";
        case MV_TRANSFER_OUT:  return "transfert_out";
        case MV_ANNULATION:    return "annulation";
    }
    return "?";
}

static void set_err(char *err_out, size_t err_len, const char *msg) {
    if (err_out && err_len) snprintf(err_out, err_len, "%s", msg);
}

/* Recomputes total_quantity for one in-memory product from the stock table.
 * Called after any movement so the dashboard/list never shows stale totals. */
static void refresh_product_total(int product_id) {
    for (int i = 0; i < g_product_count; i++) {
        if (g_products[i].id != product_id) continue;
        sqlite3_stmt *st;
        sqlite3_prepare_v2(g_db->handle,
            "SELECT COALESCE(SUM(quantity),0) FROM stock WHERE product_id=?;",
            -1, &st, NULL);
        sqlite3_bind_int(st, 1, product_id);
        int total = 0;
        if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        g_products[i].total_quantity = total;
        return;
    }
}

static void index_product(Product *p) {
    ht_put(&g_by_sku, p->sku, p);
    if (p->barcode[0]) ht_put(&g_by_barcode, p->barcode, p);
}

bool inv_init(WmsDb *db) {
    g_db = db;
    g_product_count = 0;
    ht_init(&g_by_sku, 4096);
    ht_init(&g_by_barcode, 4096);

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(g_db->handle,
        "SELECT id, prd_number, sku, barcode, name, category, unit, "
        "unit_price, alert_threshold, supplier_id, photo_path, version "
        "FROM products ORDER BY id;", -1, &st, NULL);
    if (rc != SQLITE_OK) return false;

    while (sqlite3_step(st) == SQLITE_ROW && g_product_count < WMS_MAX_PRODUCTS) {
        Product *p = &g_products[g_product_count++];
        memset(p, 0, sizeof(*p));
        p->id = sqlite3_column_int(st, 0);
        snprintf(p->prd_number, sizeof p->prd_number, "%s", sqlite3_column_text(st, 1) ? (const char*)sqlite3_column_text(st, 1) : "");
        snprintf(p->sku, sizeof p->sku, "%s", (const char*)sqlite3_column_text(st, 2));
        const unsigned char *bc = sqlite3_column_text(st, 3);
        if (bc) snprintf(p->barcode, sizeof p->barcode, "%s", (const char*)bc);
        snprintf(p->name, sizeof p->name, "%s", (const char*)sqlite3_column_text(st, 4));
        const unsigned char *cat = sqlite3_column_text(st, 5);
        if (cat) snprintf(p->category, sizeof p->category, "%s", (const char*)cat);
        snprintf(p->unit, sizeof p->unit, "%s", (const char*)sqlite3_column_text(st, 6));
        p->unit_price       = sqlite3_column_double(st, 7);
        p->alert_threshold  = sqlite3_column_int(st, 8);
        p->supplier_id      = sqlite3_column_int(st, 9);
        const unsigned char *ph = sqlite3_column_text(st, 10);
        if (ph) snprintf(p->photo_path, sizeof p->photo_path, "%s", (const char*)ph);
        p->version = sqlite3_column_int(st, 11);
        index_product(p);
    }
    sqlite3_finalize(st);

        for (int i = 0; i < g_product_count; i++)
        refresh_product_total(g_products[i].id);

    g_category_count = db_list_categories(g_db, g_categories, WMS_MAX_CATEGORIES);

    return true;
}

int inv_get_categories(Category **out) {
    *out = g_categories;
    return g_category_count;
}

void inv_refresh_categories(WmsDb *db) {
    g_category_count = db_list_categories(db, g_categories, WMS_MAX_CATEGORIES);
}

void inv_shutdown(void) {
    ht_free(&g_by_sku);
    ht_free(&g_by_barcode);
    g_product_count = 0;
}

bool inv_add_product(const Product *in, char *err_out, size_t err_len) {
    if (g_product_count >= WMS_MAX_PRODUCTS) {
        set_err(err_out, err_len, "Capacite maximale du catalogue atteinte");
        return false;
    }
    if (!db_begin_immediate_retry(g_db, 8)) {
        set_err(err_out, err_len, "Impossible d'obtenir le verrou d'ecriture (reessayez)");
        return false;
    }

    char prd[16];
    snprintf(prd, sizeof prd, "PRD-%03d", g_product_count + 1);

    sqlite3_stmt *st;
        sqlite3_prepare_v2(g_db->handle,
        "INSERT INTO products (prd_number, sku, barcode, name, category, category_id, "
        "unit, unit_price, alert_threshold, supplier_id, photo_path) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, prd, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, in->sku, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, in->barcode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, in->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, in->category, -1, SQLITE_TRANSIENT);
    if (in->category_id > 0) sqlite3_bind_int(st, 6, in->category_id);
    else sqlite3_bind_null(st, 6);
    sqlite3_bind_text(st, 7, in->unit[0] ? in->unit : "pcs", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 8, in->unit_price);
    sqlite3_bind_int(st, 9, in->alert_threshold);
    if (in->supplier_id > 0) sqlite3_bind_int(st, 10, in->supplier_id);
    else sqlite3_bind_null(st, 10);
    sqlite3_bind_text(st, 11, in->photo_path, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle)); /* e.g. UNIQUE constraint on sku */
        db_rollback(g_db);
        return false;
    }

    int new_id = (int)sqlite3_last_insert_rowid(g_db->handle);
    db_commit(g_db);

    Product *p = &g_products[g_product_count++];
    *p = *in;
    p->id = new_id;
    p->version = 0;
    p->total_quantity = 0;
    snprintf(p->prd_number, sizeof p->prd_number, "%s", prd);
    index_product(p);
    return true;
}

bool inv_update_product(const Product *in, char *err_out, size_t err_len) {
    if (!db_begin_immediate_retry(g_db, 8)) {
        set_err(err_out, err_len, "Verrou d'ecriture indisponible, reessayez");
        return false;
    }

    /* Optimistic concurrency: only succeeds if `version` still matches what
     * this client last read. Prevents two managers from silently
     * clobbering each other's edits to price/threshold/etc. */
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db->handle,
        "UPDATE products SET name=?, category=?, unit_price=?, "
        "alert_threshold=?, supplier_id=?, barcode=?, photo_path=?, "
        "version=version+1, updated_at=datetime('now') "
        "WHERE id=? AND version=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, in->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, in->category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, in->unit_price);
    sqlite3_bind_int(st, 4, in->alert_threshold);
    if (in->supplier_id > 0) sqlite3_bind_int(st, 5, in->supplier_id);
    else sqlite3_bind_null(st, 5);
    sqlite3_bind_text(st, 6, in->barcode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, in->photo_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 8, in->id);
    sqlite3_bind_int(st, 9, in->version);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
        db_rollback(g_db);
        return false;
    }
    if (sqlite3_changes(g_db->handle) == 0) {
        set_err(err_out, err_len,
            "Conflit de modification: un autre utilisateur a deja modifie ce produit. Rechargez et reessayez.");
        db_rollback(g_db);
        return false;
    }
    db_commit(g_db);

    for (int i = 0; i < g_product_count; i++) {
        if (g_products[i].id == in->id) {
            /* keep sku/prd_number immutable, refresh everything else */
            Product *p = &g_products[i];
            snprintf(p->name, sizeof p->name, "%s", in->name);
            snprintf(p->category, sizeof p->category, "%s", in->category);
            p->unit_price = in->unit_price;
            p->alert_threshold = in->alert_threshold;
            p->supplier_id = in->supplier_id;
            snprintf(p->barcode, sizeof p->barcode, "%s", in->barcode);
            snprintf(p->photo_path, sizeof p->photo_path, "%s", in->photo_path);
            p->version++;
            index_product(p);
            break;
        }
    }
    return true;
}

bool inv_delete_product(int product_id, const Session *session, char *err_out, size_t err_len) {

    if (!session_can(session, "product.delete")) {
    snprintf(err_out, err_len, "Permission refusée pour ce rôle");
    return false;
}
    if (!db_begin_immediate_retry(g_db, 8)) {

        set_err(err_out, err_len, "Verrou d'ecriture indisponible");
        return false;
    }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db->handle,
        "SELECT COALESCE(SUM(quantity),0) FROM stock WHERE product_id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    int remaining = 0;
    if (sqlite3_step(st) == SQLITE_ROW) remaining = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (remaining > 0) {
        set_err(err_out, err_len, "Stock restant non nul: videz le stock avant suppression");
        db_rollback(g_db);
        return false;
    }

    sqlite3_prepare_v2(g_db->handle, "DELETE FROM products WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
        db_rollback(g_db);
        return false;
    }
    db_commit(g_db);

    for (int i = 0; i < g_product_count; i++) {
        if (g_products[i].id == product_id) {
            ht_remove(&g_by_sku, g_products[i].sku);
            if (g_products[i].barcode[0]) ht_remove(&g_by_barcode, g_products[i].barcode);
            g_products[i] = g_products[g_product_count - 1];
            g_product_count--;
            /* re-index the moved element (pointer identity changed) */
            if (i < g_product_count) index_product(&g_products[i]);
            break;
        }
    }
    return true;
}

bool inv_delete_category(int category_id, const Session *session,
                          char *err_out, size_t err_len) {
    if (!session_can(session, "category.delete")) {
        set_err(err_out, err_len, "Permission refusee pour ce role");
        return false;
    }

    int in_use = db_count_products_in_category(g_db, category_id);
    if (in_use > 0) {
        snprintf(err_out, err_len,
                 "Impossible: %d produit(s) utilisent encore cette categorie", in_use);
        return false;
    }

    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db->handle, "DELETE FROM categories WHERE id = ?1;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, category_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
    return ok;
}

/*
 * THE atomic stock write. Everything about race-condition safety lives
 * here:
 *   1. BEGIN IMMEDIATE grabs the write lock up front (see database.c).
 *   2. The UPDATE is `quantity = quantity + delta`, computed by SQLite
 *      itself in one step - no read-modify-write gap in our C code.
 *   3. WHERE ... AND quantity + delta >= 0 rejects the write atomically
 *      if it would ever go negative (instead of checking-then-writing).
 *   4. The movement audit row is inserted in the SAME transaction, so a
 *      crash can never leave a stock change without its log entry.
 */
bool inv_post_movement(int product_id, int location_id, int delta,
                        MovementType type, const char *reference,
                        const Session *session, const char *reason,
                        char *err_out, size_t err_len) {

    if (!session_can(session, "movement.post")) {
    snprintf(err_out, err_len, "Permission refusée pour ce rôle");
    return false;
}
    if (!db_begin_immediate_retry(g_db, 8)) {
        set_err(err_out, err_len, "Verrou d'ecriture indisponible, reessayez");
        return false;
    }



            sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db->handle,
        "INSERT INTO stock (product_id, location_id, quantity) VALUES (?,?,0) "
        "ON CONFLICT(product_id, location_id) DO NOTHING;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    sqlite3_bind_int(st, 2, location_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

       sqlite3_prepare_v2(g_db->handle,
        "UPDATE stock SET quantity = quantity + ? "
        "WHERE product_id = ? AND location_id = ? AND quantity + ? >= 0;",
        -1, &st, NULL);
    sqlite3_bind_int(st, 1, delta);
    sqlite3_bind_int(st, 2, product_id);
    sqlite3_bind_int(st, 3, location_id);
    sqlite3_bind_int(st, 4, delta);
    int rc = sqlite3_step(st);
    int changes_after_update = sqlite3_changes(g_db->handle);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
        db_rollback(g_db);
        return false;
    }
    if (changes_after_update == 0) {
        set_err(err_out, err_len, "Stock insuffisant a cet emplacement");
        db_rollback(g_db);
        return false;
    }

    sqlite3_prepare_v2(g_db->handle,
        "INSERT INTO movements (product_id, location_id, delta, type, "
        "reference, user_id, reason) VALUES (?,?,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    sqlite3_bind_int(st, 2, location_id);
    sqlite3_bind_int(st, 3, delta);
    sqlite3_bind_text(st, 4, movement_type_str(type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, reference ? reference : "", -1, SQLITE_TRANSIENT);
    if (session->user_id > 0) sqlite3_bind_int(st, 6, session->user_id);
    else sqlite3_bind_null(st, 6);
    sqlite3_bind_text(st, 7, reason ? reason : "", -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
        db_rollback(g_db);
        return false;
    }

    db_commit(g_db);
    refresh_product_total(product_id);
    return true;
}

bool inv_transfer(int product_id, int from_location_id, int to_location_id,
                   int qty, const Session *session, char *err_out, size_t err_len) {
    char ref[32];
    snprintf(ref, sizeof ref, "TR-%d-%d", from_location_id, to_location_id);

    if (!inv_post_movement(product_id, from_location_id, -qty,
                            MV_TRANSFER_OUT, ref, session, NULL, err_out, err_len))
        return false;

    if (!inv_post_movement(product_id, to_location_id, qty,
                            MV_TRANSFER_IN, ref, session, NULL, err_out, err_len)) {
        char comp_err[128];
        inv_post_movement(product_id, from_location_id, qty, MV_ANNULATION,
                           ref, session, "compensation transfert echoue",
                           comp_err, sizeof comp_err);
        return false;
    }
    return true;
}

Product *inv_find_by_sku(const char *sku) {
    return (Product *)ht_get(&g_by_sku, sku);
}

Product *inv_find_by_barcode(const char *barcode) {
    return (Product *)ht_get(&g_by_barcode, barcode);
}

static bool ci_contains(const char *haystack, const char *needle) {
    if (!haystack[0] || !needle[0]) return false;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32; /* to lowercase */
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == nlen) return true; /* matched all of needle */
    }
    return false;
}

int inv_search(const char *query, Product **out, int max_results) {
    int found = 0;
    if (!query || !query[0]) return 0;

    /* exact SKU/barcode hit first via O(1) hash - typical for scanner input */
    Product *exact = inv_find_by_sku(query);
    if (exact && found < max_results) out[found++] = exact;

    for (int i = 0; i < g_product_count && found < max_results; i++) {
        Product *p = &g_products[i];
        if (p == exact) continue;
        if (ci_contains(p->name, query) || ci_contains(p->sku, query) ||
            ci_contains(p->barcode, query)) {
            out[found++] = p;
        }
    }
    return found;
}

int inv_all_products(Product **out_array) {
    *out_array = g_products;
    return g_product_count;
}
