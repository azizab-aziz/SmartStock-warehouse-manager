#include "db.h"
#include "core.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>

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

    /* TODO: replace with real login screen (users table + password hash
       check). For now we run as a single hardcoded admin session so the
       GUI/inventory wiring can be exercised end-to-end. */
        gui_run(&db);

    inv_shutdown();
    db_close(&db);
    return 0;
}
