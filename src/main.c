#include "db.h"
#include "core.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>

int main(void) {
    WmsDb db;

    /* saves/ is created next to the executable, matching the structure
       described in the spec (saves/historique, saves/backup, saves/exports) */
    system("mkdir saves 2>nul");
    system("mkdir saves\\historique 2>nul");
    system("mkdir saves\\backup 2>nul");
    system("mkdir saves\\exports 2>nul");

    if (!db_open(&db, "saves/warehouse.db")) {
        fprintf(stderr, "Impossible d'ouvrir la base de donnees.\n");
        return 1;
    }
    /* schema.sql is copied next to the executable by CMake's POST_BUILD
       step (see CMakeLists.txt) so this relative path works from wherever
       you run the built binary from inside build/. */
    if (!db_apply_schema(&db, "schema.sql")) {
        fprintf(stderr, "Impossible d'appliquer le schema.\n");
        db_close(&db);
        return 1;
    }
    if (!inv_init(&db)) {
        fprintf(stderr, "Impossible d'initialiser l'inventaire.\n");
        db_close(&db);
        return 1;
    }

        /* Ensure at least one location exists, since movements currently
       hardcode location_id=1 (a real location picker is a later feature). */
    sqlite3_exec(db.handle,
        "INSERT OR IGNORE INTO locations (id, code, capacity) VALUES (1, 'A-01-01', 9999);",
        NULL, NULL, NULL);

        sqlite3_stmt *dbg;
sqlite3_prepare_v2(db.handle, "SELECT id FROM locations;", -1, &dbg, NULL);
while (sqlite3_step(dbg) == SQLITE_ROW)
    printf("location id=%d exists\n", sqlite3_column_int(dbg, 0));
sqlite3_finalize(dbg);

        /* TEMPORARY - seed one admin user, then delete this block */
    char seed_err[256];
    if (db_create_user(&db, "medaz", "MotDePasse123!", "admin", seed_err, sizeof(seed_err))) {
        printf("Utilisateur cree avec succes !\n");
    } else {
        printf("Erreur creation utilisateur: %s\n", seed_err);
    }

    /* TODO: replace with real login screen (users table + password hash
       check). For now we run as a single hardcoded admin session so the
       GUI/inventory wiring can be exercised end-to-end. */
        gui_run(&db);

    inv_shutdown();
    db_close(&db);
    return 0;
}
