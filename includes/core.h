#ifndef WMS_CORE_H
#define WMS_CORE_H

#include "db.h"
#include "session.h"
#include <stdbool.h>
#include <stddef.h>

#define WMS_MAX_PRODUCTS 65536   /* in-memory index capacity, grow if needed */

typedef struct {
    int    id;
    char   prd_number[16];
    char   sku[64];
    char   barcode[64];
    char   name[128];
    char   category[64];
    int    category_id;   /* 0 = uncategorized; FK into categories table */
    char   unit[16];
    double unit_price;
    int    alert_threshold;
    int    supplier_id;
    char   photo_path[256];
    int    version;
    int    total_quantity;      /* denormalized sum across all locations,
                                    refreshed on load/movement for fast
                                    dashboard reads without a JOIN */
} Product;

typedef struct {
    int  id;
    char code[16];   /* A-01-02 */
    char aisle[16];
    char shelf[16];
    char bin[16];
    int  capacity;
} Location;
typedef struct {
    int    id;
    int    product_id;
    char   product_name[128]; /* only filled by inv_get_all_movements() - "Produit supprime" if gone */
    int    location_id;
    int    delta;         /* positive = in, negative = out */
    char   type[24];       /* "reception", "expedition", etc. - see movement_type_str() */
    char   reference[64];
    int    user_id;
    char   username[64];   /* joined from users, empty if unknown/deleted */
    char   reason[128];
    char   created_at[32]; /* "YYYY-MM-DD HH:MM:SS", from SQLite datetime('now') */
} Movement;
typedef enum {
    MV_RECEPTION, MV_RETOUR, MV_ADJUST_POS,
    MV_EXPEDITION, MV_PERTE, MV_ADJUST_NEG,
    MV_TRANSFER_IN, MV_TRANSFER_OUT, MV_ANNULATION
} MovementType;

/* ---- Inventory subsystem: owns the in-memory index + wraps every write
 *      in a proper transaction. Call inv_init() once at startup. ---- */

bool inv_init(WmsDb *db);
void inv_shutdown(void);

/* Product CRUD. add/update/delete all update BOTH sqlite and the
 * in-memory hash index inside the same call so they can never drift. */
bool inv_add_product(const Product *p, char *err_out, size_t err_len);
bool inv_update_product(const Product *p, char *err_out, size_t err_len); /* uses p->version for optimistic lock */
bool inv_delete_product(int product_id, const Session *session, char *err_out, size_t err_len);
/*
 * The core anti-race-condition primitive. Applies `delta` atomically
 * (positive = stock in, negative = stock out) to product_id@location_id,
 * writes the audit-log movement row, in the SAME transaction.
 * Returns false (and rolls back) if resulting quantity would go negative,
 * or if the write lock couldn't be acquired after retrying.
 */
bool inv_post_movement(int product_id, int location_id, int delta, MovementType type,
                        const char *reference, const Session *session,
                        const char *reason, char *err_out, size_t err_len);

bool inv_transfer(int product_id, int from_location_id, int to_location_id,
                   int qty, const Session *session, char *err_out, size_t err_len);
/* Movement history for one product, most recent first (joins the
 * username in). Returns count written into `out`, up to max_results. */
int inv_get_movements(int product_id, Movement *out, int max_results);
/* Every movement across every product, most recent first, product name
 * joined in - the full audit trail (not scoped to one product). */
int inv_get_all_movements(Movement *out, int max_results);
/* ---- Fast in-memory lookups (O(1) hash, no DB round-trip) ---- */
Product *inv_find_by_sku(const char *sku);
Product *inv_find_by_barcode(const char *barcode);

/* Case-insensitive substring match over name+sku+barcode, fills `out`
 * (caller-allocated array) up to max_results, returns count found. */
int inv_search(const char *query, Product **out, int max_results);

/* Snapshot access for the GUI's product table / pagination. */
int inv_all_products(Product **out_array); /* returns count, *out_array points at internal buffer, do not free */

int  inv_get_categories(Category **out);
void inv_refresh_categories(WmsDb *db);

/* Deletes a category only if no product currently references it (cascade
 * safety). On refusal, err_out lists how many products are still linked. */
bool inv_delete_category(int category_id, const Session *session,
                          char *err_out, size_t err_len);
/* Writes active products to a CSV for the Excel-export pipeline.
 * category_id <= 0 exports every category; otherwise only that one.
 * Columns: PRD,SKU,Nom,Categorie,Unite,Quantite,Prix_Unitaire,
 *          Valeur_Stock,Seuil_Alerte,Statut */
bool inv_export_csv(int category_id, const char *path, char *err_out, size_t err_len);
/* Single-product CSV (same columns as inv_export_csv) - feeds the PDF
 * product sheet's info panel. Returns false if the product id isn't found. */
bool inv_export_product_info_csv(int product_id, const char *path, char *err_out, size_t err_len);
/* Movement history for one product, written as CSV (Date,Qte,Type,
 * Reference,Utilisateur,Raison) - feeds the PDF sheet's history table. */
bool inv_export_movements_csv(int product_id, const char *path, int max_results,
                               char *err_out, size_t err_len);
#endif
