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

typedef struct {
    int  id;
    char code[16];   /* e.g. "A-01-02" */
    char aisle[16];
    char shelf[16];
    char bin[16];
    int  capacity;
} Location;

int  db_list_categories(WmsDb *db, Category *out, int max_count);
bool db_find_or_create_category(WmsDb *db, const char *name, int *out_id);

int db_count_products_in_category(WmsDb *db, int category_id);

int  db_list_users(WmsDb *db, int *out_ids, char names[][64], char roles[][16], int max_count);
bool db_update_user_role(WmsDb *db, int user_id, const char *new_role);

bool db_update_category_name(WmsDb *db, int category_id, const char *new_name);

typedef struct {
    int  id;
    int  user_id;
    char username[64];    /* "systeme" if user_id <= 0, joined from users otherwise */
    char action[32];      /* "creation","modification","suppression","restauration" */
    char entity_type[32]; /* "produit","categorie","fournisseur","utilisateur","emplacement","commande" */
    char entity_label[128];
    char details[192];
    char created_at[32];
} AuditLogEntry;

/* Writes one CRUD audit entry - separate from the movements table, which
 * already covers stock changes on its own (see inv_get_all_movements). */
bool db_log_audit(WmsDb *db, int user_id, const char *action, const char *entity_type,
                   const char *entity_label, const char *details);
int  db_list_audit_log(WmsDb *db, AuditLogEntry *out, int max_count);

int  db_list_locations(WmsDb *db, Location *out, int max_count);
bool db_create_location(WmsDb *db, const char *code, const char *aisle, const char *shelf,
                         const char *bin, int capacity, char *err_out, size_t err_len);

typedef struct {
    int  id;
    char name[128];
    char contact_name[128];
    char phone[32];
    char email[128];
    char address[192];
} Supplier;

int  db_list_suppliers(WmsDb *db, Supplier *out, int max_count);
bool db_create_supplier(WmsDb *db, const Supplier *s, char *err_out, size_t err_len);
bool db_update_supplier(WmsDb *db, const Supplier *s, char *err_out, size_t err_len);
/* Refuses if any active product still references this supplier. */
bool db_delete_supplier(WmsDb *db, int supplier_id, char *err_out, size_t err_len);

typedef struct {
    int  id;
    int  supplier_id;
    char supplier_name[128];
    char po_number[32];       /* "PO-0001", auto-generated */
    char status[16];          /* "brouillon","commande","recu_partiel","recu","annule" */
    char reference[64];
    int  created_by;
    char created_by_name[64];
    char created_at[32];
    char received_at[32];     /* empty until status becomes "recu" */
} PurchaseOrder;

typedef struct {
    int    id;
    int    po_id;
    int    product_id;
    char   product_name[128];
    char   product_sku[64];
    int    quantity_ordered;
    int    quantity_received;
    double unit_cost;
} PurchaseOrderItem;

bool db_create_purchase_order(WmsDb *db, int supplier_id, int created_by,
                               const char *reference, int *out_po_id,
                               char *err_out, size_t err_len);
bool db_add_po_item(WmsDb *db, int po_id, int product_id, int quantity_ordered,
                    double unit_cost, char *err_out, size_t err_len);
int  db_list_purchase_orders(WmsDb *db, PurchaseOrder *out, int max_count);
int  db_get_po_items(WmsDb *db, int po_id, PurchaseOrderItem *out, int max_count);
bool db_update_po_item_received(WmsDb *db, int po_item_id, int new_received_qty,
                                 char *err_out, size_t err_len);
bool db_update_po_status(WmsDb *db, int po_id, const char *new_status);

typedef struct {
    int  id;
    char name[128];
    char contact_name[128];
    char phone[32];
    char email[128];
    char address[192];
} Customer;

int  db_list_customers(WmsDb *db, Customer *out, int max_count);
bool db_create_customer(WmsDb *db, const Customer *c, char *err_out, size_t err_len);
bool db_update_customer(WmsDb *db, const Customer *c, char *err_out, size_t err_len);
/* Refuses if any dispatch order still references this customer. */
bool db_delete_customer(WmsDb *db, int customer_id, char *err_out, size_t err_len);

typedef struct {
    int  id;
    int  customer_id;
    char customer_name[128];
    char do_number[32];       /* "BL-0001" (bon de livraison), auto-generated */
    char status[16];          /* "brouillon","confirmee","expedie_partiel","expedie","annule" */
    char reference[64];
    int  created_by;
    char created_by_name[64];
    char created_at[32];
    char shipped_at[32];      /* empty until status becomes "expedie" */
} DispatchOrder;

typedef struct {
    int    id;
    int    do_id;
    int    product_id;
    char   product_name[128];
    char   product_sku[64];
    int    quantity_ordered;
    int    quantity_shipped;
    double unit_price;
} DispatchOrderItem;

bool db_create_dispatch_order(WmsDb *db, int customer_id, int created_by,
                               const char *reference, int *out_do_id,
                               char *err_out, size_t err_len);
bool db_add_do_item(WmsDb *db, int do_id, int product_id, int quantity_ordered,
                    double unit_price, char *err_out, size_t err_len);
int  db_list_dispatch_orders(WmsDb *db, DispatchOrder *out, int max_count);
int  db_get_do_items(WmsDb *db, int do_id, DispatchOrderItem *out, int max_count);
bool db_update_do_item_shipped(WmsDb *db, int do_item_id, int new_shipped_qty,
                                 char *err_out, size_t err_len);
bool db_update_do_status(WmsDb *db, int do_id, const char *new_status);

typedef struct {
    int  id;
    int  do_id;              /* 0 if not linked to a dispatch order */
    char do_number[32];      /* empty if not linked */
    int  product_id;
    char product_name[128];
    int  quantity;
    char reason[128];
    int  processed_by;
    char processed_by_name[64];
    char created_at[32];
} ReturnRecord;

bool db_create_return(WmsDb *db, int do_id, int product_id, int quantity,
                       const char *reason, int processed_by, int *out_return_id,
                       char *err_out, size_t err_len);
int  db_list_returns(WmsDb *db, ReturnRecord *out, int max_count);

/* category_id <= 0 exports every category's dispatch/return records;
 * category_id > 0 filters to only products currently in that category. */
bool db_export_do_items_csv(WmsDb *db, const char *path, char *err_out, size_t err_len);
bool db_export_returns_csv(WmsDb *db, const char *path, char *err_out, size_t err_len);
bool db_export_suppliers_csv(WmsDb *db, const char *path, char *err_out, size_t err_len);
bool db_export_customers_csv(WmsDb *db, const char *path, char *err_out, size_t err_len);
bool db_export_po_items_csv(WmsDb *db, const char *path, char *err_out, size_t err_len);

#endif
