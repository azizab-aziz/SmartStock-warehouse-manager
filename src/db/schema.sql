-- storage-wms schema.sql
-- Applied once at first launch (database.c checks PRAGMA user_version)

PRAGMA journal_mode = WAL;      -- readers never block writers
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 3000;     -- ms to wait on a lock before SQLITE_BUSY

CREATE TABLE IF NOT EXISTS categories (
    id   INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL COLLATE NOCASE   -- COLLATE NOCASE = case-insensitive uniqueness
);

CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,   -- libsodium crypto_pwhash_str output
    role          TEXT NOT NULL CHECK(role IN ('admin','manager','operateur')),
    active        INTEGER NOT NULL DEFAULT 1,
    created_at    TEXT NOT NULL DEFAULT (datetime('now')),
    last_login_at TEXT
);
CREATE TABLE IF NOT EXISTS login_log (
    id         INTEGER PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id),
    login_at   TEXT NOT NULL DEFAULT (datetime('now')),
    logout_at  TEXT,
    duration_s INTEGER
);

CREATE TABLE IF NOT EXISTS locations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    code        TEXT UNIQUE NOT NULL,   -- e.g. A-01-02  (aisle-shelf-bin)
    aisle       TEXT NOT NULL,
    shelf       TEXT NOT NULL,
    bin         TEXT NOT NULL,
    capacity    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_locations_code ON locations(code);

CREATE TABLE IF NOT EXISTS suppliers (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL,
    contact     TEXT,
    lead_time_days INTEGER DEFAULT 0,
    terms       TEXT
);

CREATE TABLE IF NOT EXISTS products (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    prd_number      TEXT UNIQUE NOT NULL,   -- PRD-001, PRD-002 ...
    sku             TEXT UNIQUE NOT NULL,
    barcode         TEXT,
    name            TEXT NOT NULL,
    category        TEXT,
    unit            TEXT NOT NULL DEFAULT 'pcs',
    unit_price      REAL NOT NULL DEFAULT 0,
    alert_threshold INTEGER NOT NULL DEFAULT 0,
    supplier_id     INTEGER REFERENCES suppliers(id),
    photo_path      TEXT,
    version         INTEGER NOT NULL DEFAULT 0,   -- optimistic-lock counter
    created_at      TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_products_sku      ON products(sku);
CREATE INDEX IF NOT EXISTS idx_products_barcode  ON products(barcode);
CREATE INDEX IF NOT EXISTS idx_products_name     ON products(name);

-- Current quantity of a product AT a specific location.
-- This is the row every movement atomically increments/decrements.
CREATE TABLE IF NOT EXISTS stock (
    product_id  INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
    location_id INTEGER NOT NULL REFERENCES locations(id) ON DELETE CASCADE,
    quantity    INTEGER NOT NULL DEFAULT 0 CHECK(quantity >= 0),
    PRIMARY KEY (product_id, location_id)
);
CREATE INDEX IF NOT EXISTS idx_stock_location ON stock(location_id, product_id);
CREATE INDEX IF NOT EXISTS idx_stock_product  ON stock(product_id, location_id);

-- Every single movement, ever. Immutable (cancellation = new reversing row).
CREATE TABLE IF NOT EXISTS movements (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    product_id  INTEGER NOT NULL REFERENCES products(id),
    location_id INTEGER NOT NULL REFERENCES locations(id),
    delta       INTEGER NOT NULL,      -- +in / -out
    type        TEXT NOT NULL CHECK(type IN
                 ('reception','retour','ajustement+','expedition','perte',
                  'ajustement-','transfert_in','transfert_out','annulation')),
    reference   TEXT,                  -- PO#/SO#
    user_id     INTEGER REFERENCES users(id),
    reason      TEXT,                  -- required for annulation/ajustement
    created_at  TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_movements_product  ON movements(product_id, created_at);
CREATE INDEX IF NOT EXISTS idx_movements_location ON movements(location_id, created_at);

CREATE TABLE IF NOT EXISTS purchase_orders (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    po_number   TEXT UNIQUE NOT NULL,
    supplier_id INTEGER NOT NULL REFERENCES suppliers(id),
    status      TEXT NOT NULL DEFAULT 'en_attente'
                CHECK(status IN ('en_attente','partiellement_recu','complete')),
    created_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS purchase_order_lines (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    po_id           INTEGER NOT NULL REFERENCES purchase_orders(id) ON DELETE CASCADE,
    product_id      INTEGER NOT NULL REFERENCES products(id),
    qty_ordered     INTEGER NOT NULL,
    qty_received    INTEGER NOT NULL DEFAULT 0
);

PRAGMA user_version = 1;
