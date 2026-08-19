#include "db.h"
#include <stdio.h>

int main(void) {
    WmsDb db;
    if (!db_open(&db, "saves/warehouse.db")) {
        fprintf(stderr, "Impossible d'ouvrir la base\n");
        return 1;
    }

    char err[256];
    if (db_create_user(&db, "medaziz", "2002abidi", "admin", err, sizeof(err))) {
        printf("Utilisateur cree avec succes !\n");
    } else {
        printf("Erreur: %s\n", err);
    }

    db_close(&db);
    return 0;
}
