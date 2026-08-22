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
   char *loc_err = NULL;
int loc_rc = sqlite3_exec(db.handle,
    "INSERT OR IGNORE INTO locations (id, code, capacity) VALUES (1, 'A-01-01', 9999);",
    NULL, NULL, &loc_err);

FILE *loc_log = fopen("debug_insert.txt", "w");
fprintf(loc_log, "rc=%d changes=%d err=%s\n", loc_rc, sqlite3_changes(db.handle),
        loc_err ? loc_err : "(none)");

/* NEW: immediately re-check if it's really there */
sqlite3_stmt *verify;
sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM locations;", -1, &verify, NULL);
sqlite3_step(verify);
fprintf(loc_log, "total locations right after insert = %d\n", sqlite3_column_int(verify, 0));
sqlite3_finalize(verify);

if (loc_err) sqlite3_free(loc_err);
fclose(loc_log);
if (loc_err) sqlite3_free(loc_err);
fclose(loc_log);

            FILE *dbg_log = fopen("debug_locations.txt", "w");
    sqlite3_stmt *dbg;
    sqlite3_prepare_v2(db.handle, "SELECT id, code FROM locations;", -1, &dbg, NULL);
    while (sqlite3_step(dbg) == SQLITE_ROW)
        fprintf(dbg_log, "location id=%d code=%s\n", sqlite3_column_int(dbg,0), sqlite3_column_text(dbg,1));
    fclose(dbg_log);



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
