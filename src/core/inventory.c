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
#define WMS_MAX_ARCHIVED 4096
static Product g_archived[WMS_MAX_ARCHIVED];
static int     g_archived_count = 0;

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
        "unit_price, alert_threshold, supplier_id, photo_path, version, category_id "
        "FROM products WHERE active = 1 ORDER BY id;", -1, &st, NULL);
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
        p->category_id = sqlite3_column_int(st, 12);
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

        /* Generate from the database's real max, not the in-memory active
       count - soft-deleted products still occupy their prd_number
       forever, so counting only active rows can collide with an old,
       inactive product's number. */
    int next_num = 1;
    sqlite3_stmt *num_st;
    sqlite3_prepare_v2(g_db->handle,
        "SELECT COALESCE(MAX(CAST(SUBSTR(prd_number, 5) AS INTEGER)), 0) + 1 FROM products;",
        -1, &num_st, NULL);
    if (sqlite3_step(num_st) == SQLITE_ROW) next_num = sqlite3_column_int(num_st, 0);
    sqlite3_finalize(num_st);

    char prd[16];
    snprintf(prd, sizeof prd, "PRD-%03d", next_num);

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

        /* Products have audit history (movements) that must be preserved for
       traceability even after the product itself is removed. Rather than
       deleting movement rows (which would destroy the audit trail), null
       out the reference so the FK is satisfied but the historical record
       survives. Requires movements.product_id to allow NULL - see schema
       note below if this still fails. */
    sqlite3_prepare_v2(g_db->handle,
        "UPDATE movements SET product_id = NULL WHERE product_id = ?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    /* Also clear any now-empty stock rows for this product (should already
       be 0 quantity per the check above, but the FK still needs clearing). */
    sqlite3_prepare_v2(g_db->handle, "DELETE FROM stock WHERE product_id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

        /* Soft delete: products carry audit history via movements.product_id
       (NOT NULL FK), so a hard DELETE always violates that constraint.
       Marking inactive preserves the audit trail and sidesteps the FK
       issue entirely - same pattern as users.active. */
        /* Soft delete: mark inactive AND clear category_id, so an inactive
       product can never block a category deletion via the FK constraint
       - the audit trail (movements) is preserved, but the category link
       itself is no longer "in use" once the product is gone from view. */
    sqlite3_prepare_v2(g_db->handle,
        "UPDATE products SET active = 0, category_id = NULL WHERE id=?;", -1, &st, NULL);
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

    /* Business rule: only ACTIVE products block deletion. */
    int in_use = db_count_products_in_category(g_db, category_id);
    if (in_use > 0) {
        snprintf(err_out, err_len,
                 "Impossible: %d produit(s) utilisent encore cette categorie", in_use);
        return false;
    }

    /* Cleanup: clear category_id on EVERY product still pointing at this
       category, including already-inactive ones from before this cleanup
       step existed. Without this, old soft-deleted rows silently violate
       the FK the moment we try to delete the category, even though the
       business-rule check above already said it was safe. */
    sqlite3_stmt *clr;
    sqlite3_prepare_v2(g_db->handle,
        "UPDATE products SET category_id = NULL WHERE category_id = ?1;", -1, &clr, NULL);
    sqlite3_bind_int(clr, 1, category_id);
    sqlite3_step(clr);
    sqlite3_finalize(clr);

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

/* Doubles any embedded quote so the field stays valid CSV, and wraps in
 * quotes whenever the raw text contains a quote or a comma (names and
 * categories can legally contain both). */
static void csv_write_field(FILE *f, const char *text) {
    bool needs_quotes = (strchr(text, ',') != NULL) || (strchr(text, '"') != NULL);
    if (!needs_quotes) {
        fputs(text, f);
        return;
    }
    fputc('"', f);
    for (const char *c = text; *c; c++) {
        if (*c == '"') fputc('"', f); /* double it */
        fputc(*c, f);
    }
    fputc('"', f);
}

bool inv_export_csv(int category_id, const char *path, char *err_out, size_t err_len) {
    FILE *f = fopen(path, "wb"); /* binary mode - we're writing raw UTF-8 bytes, not text-mode-translated */
    if (!f) {
        set_err(err_out, err_len, "Impossible de creer le fichier CSV (dossier manquant ?)");
        return false;
    }

    /* UTF-8 BOM - without this, Excel guesses the system codepage instead
       of UTF-8 and accented characters (é, è, etc.) render as garbage.
       The Python script's utf-8-sig reader already strips this correctly. */
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);

    fprintf(f, "PRD,SKU,Nom,Categorie,Unite,Quantite,Prix_Unitaire,Valeur_Stock,Seuil_Alerte,Statut\r\n");
    for (int i = 0; i < g_product_count; i++) {
        Product *p = &g_products[i];
        if (category_id > 0 && p->category_id != category_id) continue;

        double stock_value = (double)p->total_quantity * p->unit_price;
        const char *statut = (p->total_quantity <= p->alert_threshold) ? "STOCK FAIBLE" : "OK";

        csv_write_field(f, p->prd_number); fputc(',', f);
        csv_write_field(f, p->sku); fputc(',', f);
        csv_write_field(f, p->name); fputc(',', f);
        csv_write_field(f, p->category); fputc(',', f);
        csv_write_field(f, p->unit); fputc(',', f);
                fprintf(f, "%d,%.2f,%.2f,%d,%s\r\n",
                p->total_quantity, p->unit_price, stock_value, p->alert_threshold, statut);
    }

    fclose(f);
    return true;
}

static Product *find_product_internal(int product_id) {
    for (int i = 0; i < g_product_count; i++)
        if (g_products[i].id == product_id) return &g_products[i];
    return NULL;
}

bool inv_export_product_info_csv(int product_id, const char *path, char *err_out, size_t err_len) {
    Product *p = find_product_internal(product_id);
    if (!p) {
        set_err(err_out, err_len, "Produit introuvable");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        set_err(err_out, err_len, "Impossible de creer le fichier CSV (dossier manquant ?)");
        return false;
    }
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);
    fprintf(f, "PRD,SKU,Nom,Categorie,Unite,Quantite,Prix_Unitaire,Valeur_Stock,Seuil_Alerte,Statut\r\n");

    double stock_value = (double)p->total_quantity * p->unit_price;
    const char *statut = (p->total_quantity <= p->alert_threshold) ? "STOCK FAIBLE" : "OK";

    csv_write_field(f, p->prd_number); fputc(',', f);
    csv_write_field(f, p->sku); fputc(',', f);
    csv_write_field(f, p->name); fputc(',', f);
    csv_write_field(f, p->category); fputc(',', f);
    csv_write_field(f, p->unit); fputc(',', f);
    fprintf(f, "%d,%.2f,%.2f,%d,%s\r\n",
            p->total_quantity, p->unit_price, stock_value, p->alert_threshold, statut);

    fclose(f);
    return true;
}

bool inv_export_movements_csv(int product_id, const char *path, int max_results,
                               char *err_out, size_t err_len) {
    Movement *movs = malloc(sizeof(Movement) * (size_t)max_results);
    if (!movs) {
        set_err(err_out, err_len, "Memoire insuffisante");
        return false;
    }
    int count = inv_get_movements(product_id, movs, max_results);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(movs);
        set_err(err_out, err_len, "Impossible de creer le fichier CSV (dossier manquant ?)");
        return false;
    }
    fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);
    fprintf(f, "Date,Qte,Type,Reference,Utilisateur,Raison\r\n");

    for (int i = 0; i < count; i++) {
        Movement *mv = &movs[i];
        csv_write_field(f, mv->created_at); fputc(',', f);
        fprintf(f, "%+d,", mv->delta);
        csv_write_field(f, mv->type); fputc(',', f);
        csv_write_field(f, mv->reference); fputc(',', f);
        csv_write_field(f, mv->username[0] ? mv->username : "-"); fputc(',', f);
        csv_write_field(f, mv->reason);
        fprintf(f, "\r\n");
    }

    fclose(f);
    free(movs);
    return true;
}

int inv_get_movements(int product_id, Movement *out, int max_results) {
    int count = 0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(g_db->handle,
        "SELECT m.id, m.product_id, m.location_id, m.delta, m.type, "
        "m.reference, m.user_id, COALESCE(u.username, ''), m.reason, m.created_at "
        "FROM movements m LEFT JOIN users u ON u.id = m.user_id "
        "WHERE m.product_id = ? ORDER BY m.created_at DESC, m.id DESC;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, product_id);

    while (count < max_results && sqlite3_step(st) == SQLITE_ROW) {
        Movement *mv = &out[count++];
        memset(mv, 0, sizeof(*mv));
        mv->id          = sqlite3_column_int(st, 0);
        mv->product_id  = sqlite3_column_int(st, 1);
        mv->location_id = sqlite3_column_int(st, 2);
        mv->delta       = sqlite3_column_int(st, 3);
        snprintf(mv->type, sizeof mv->type, "%s", (const char*)sqlite3_column_text(st, 4));
        const unsigned char *ref = sqlite3_column_text(st, 5);
        if (ref) snprintf(mv->reference, sizeof mv->reference, "%s", (const char*)ref);
        mv->user_id = sqlite3_column_int(st, 6);
        snprintf(mv->username, sizeof mv->username, "%s", (const char*)sqlite3_column_text(st, 7));
        const unsigned char *reason = sqlite3_column_text(st, 8);
        if (reason) snprintf(mv->reason, sizeof mv->reason, "%s", (const char*)reason);
                snprintf(mv->created_at, sizeof mv->created_at, "%s", (const char*)sqlite3_column_text(st, 9));
    }
    sqlite3_finalize(st);
    return count;
}

int inv_get_all_movements(Movement *out, int max_results) {
    int count = 0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(g_db->handle,
        "SELECT m.id, m.product_id, m.location_id, m.delta, m.type, "
        "m.reference, m.user_id, COALESCE(u.username, ''), m.reason, m.created_at, "
        "COALESCE(p.name, 'Produit supprime') "
        "FROM movements m LEFT JOIN users u ON u.id = m.user_id "
        "LEFT JOIN products p ON p.id = m.product_id "
        "ORDER BY m.created_at DESC, m.id DESC;",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;

    while (count < max_results && sqlite3_step(st) == SQLITE_ROW) {
        Movement *mv = &out[count++];
        memset(mv, 0, sizeof(*mv));
        mv->id          = sqlite3_column_int(st, 0);
        mv->product_id  = sqlite3_column_int(st, 1);
        mv->location_id = sqlite3_column_int(st, 2);
        mv->delta       = sqlite3_column_int(st, 3);
        snprintf(mv->type, sizeof mv->type, "%s", (const char*)sqlite3_column_text(st, 4));
        const unsigned char *ref = sqlite3_column_text(st, 5);
        if (ref) snprintf(mv->reference, sizeof mv->reference, "%s", (const char*)ref);
        mv->user_id = sqlite3_column_int(st, 6);
        snprintf(mv->username, sizeof mv->username, "%s", (const char*)sqlite3_column_text(st, 7));
        const unsigned char *reason = sqlite3_column_text(st, 8);
        if (reason) snprintf(mv->reason, sizeof mv->reason, "%s", (const char*)reason);
        snprintf(mv->created_at, sizeof mv->created_at, "%s", (const char*)sqlite3_column_text(st, 9));
               snprintf(mv->product_name, sizeof mv->product_name, "%s", (const char*)sqlite3_column_text(st, 10));
    }
    sqlite3_finalize(st);
    return count;
}

int inv_get_archived_products(Product **out) {
    g_archived_count = 0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(g_db->handle,
        "SELECT id, prd_number, sku, barcode, name, category, unit, "
        "unit_price, alert_threshold, supplier_id, photo_path, version, category_id "
        "FROM products WHERE active = 0 ORDER BY name COLLATE NOCASE;", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW && g_archived_count < WMS_MAX_ARCHIVED) {
            Product *p = &g_archived[g_archived_count++];
            memset(p, 0, sizeof(*p));
            p->id = sqlite3_column_int(st, 0);
            snprintf(p->prd_number, sizeof p->prd_number, "%s", sqlite3_column_text(st, 1) ? (const char*)sqlite3_column_text(st, 1) : "");
            snprintf(p->sku, sizeof p->sku, "%s", (const char*)sqlite3_column_text(st, 2));
            const unsigned char *bc = sqlite3_column_text(st, 3);
            if (bc) snprintf(p->barcode, sizeof p->barcode, "%s", (const char*)bc);
            snprintf(p->name, sizeof p->name, "%s", (const char*)sqlite3_column_text(st, 4));
            const unsigned char *cat = sqlite3_column_text(st, 5);
            if (cat) snprintf(p->category, sizeof p->category, "%s", (const char*)cat); /* old name, kept for context */
            snprintf(p->unit, sizeof p->unit, "%s", (const char*)sqlite3_column_text(st, 6));
            p->unit_price      = sqlite3_column_double(st, 7);
            p->alert_threshold = sqlite3_column_int(st, 8);
            p->supplier_id     = sqlite3_column_int(st, 9);
            const unsigned char *ph = sqlite3_column_text(st, 10);
            if (ph) snprintf(p->photo_path, sizeof p->photo_path, "%s", (const char*)ph);
            p->version     = sqlite3_column_int(st, 11);
            p->category_id = sqlite3_column_int(st, 12); /* 0 - was cleared on archive */
            p->total_quantity = 0; /* stock rows are cleared on archive - always 0 */
        }
    }
    sqlite3_finalize(st);
    *out = g_archived;
    return g_archived_count;
}

bool inv_restore_product(int product_id, char *err_out, size_t err_len) {
    if (!db_begin_immediate_retry(g_db, 8)) {
        set_err(err_out, err_len, "Verrou d'ecriture indisponible");
        return false;
    }

    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db->handle, "UPDATE products SET active = 1 WHERE id = ?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, product_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        set_err(err_out, err_len, sqlite3_errmsg(g_db->handle));
        db_rollback(g_db);
        return false;
    }
    if (sqlite3_changes(g_db->handle) == 0) {
        set_err(err_out, err_len, "Produit introuvable");
        db_rollback(g_db);
        return false;
    }
    db_commit(g_db);

    /* Pull the now-active row back into the in-memory index, same
       column layout as inv_init()'s load query. */
    if (g_product_count < WMS_MAX_PRODUCTS) {
        sqlite3_stmt *st2;
        sqlite3_prepare_v2(g_db->handle,
            "SELECT id, prd_number, sku, barcode, name, category, unit, "
            "unit_price, alert_threshold, supplier_id, photo_path, version, category_id "
            "FROM products WHERE id = ?;", -1, &st2, NULL);
        sqlite3_bind_int(st2, 1, product_id);
        if (sqlite3_step(st2) == SQLITE_ROW) {
            Product *p = &g_products[g_product_count++];
            memset(p, 0, sizeof(*p));
            p->id = sqlite3_column_int(st2, 0);
            snprintf(p->prd_number, sizeof p->prd_number, "%s", sqlite3_column_text(st2, 1) ? (const char*)sqlite3_column_text(st2, 1) : "");
            snprintf(p->sku, sizeof p->sku, "%s", (const char*)sqlite3_column_text(st2, 2));
            const unsigned char *bc = sqlite3_column_text(st2, 3);
            if (bc) snprintf(p->barcode, sizeof p->barcode, "%s", (const char*)bc);
            snprintf(p->name, sizeof p->name, "%s", (const char*)sqlite3_column_text(st2, 4));
            const unsigned char *cat = sqlite3_column_text(st2, 5);
            if (cat) snprintf(p->category, sizeof p->category, "%s", (const char*)cat);
            snprintf(p->unit, sizeof p->unit, "%s", (const char*)sqlite3_column_text(st2, 6));
            p->unit_price      = sqlite3_column_double(st2, 7);
            p->alert_threshold = sqlite3_column_int(st2, 8);
            p->supplier_id     = sqlite3_column_int(st2, 9);
            const unsigned char *ph = sqlite3_column_text(st2, 10);
            if (ph) snprintf(p->photo_path, sizeof p->photo_path, "%s", (const char*)ph);
            p->version     = sqlite3_column_int(st2, 11);
            p->category_id = sqlite3_column_int(st2, 12); /* 0 - restored as "Sans categorie" */
            index_product(p);
        }
        sqlite3_finalize(st2);
    }
    refresh_product_total(product_id);
    return true;
}
