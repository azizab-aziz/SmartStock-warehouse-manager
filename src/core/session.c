#include "session.h"
#include <string.h>
#include <stdio.h>

bool session_login(WmsDb *db, const char *username, const char *password,
                    Session *out, char *err_out, size_t err_len) {
    int user_id;
    char role[16];

    if (!db_verify_credentials(db, username, password, &user_id, role, sizeof(role))) {
        /* Deliberately generic: don't tell an attacker whether the
         * username exists or the password was wrong. */
        snprintf(err_out, err_len, "Identifiant ou mot de passe incorrect");
        return false;
    }

    int login_log_id = 0;
    db_log_login(db, user_id, &login_log_id); /* non-fatal if it fails */

    memset(out, 0, sizeof(*out));
    out->user_id = user_id;
    snprintf(out->username, sizeof(out->username), "%s", username);
    snprintf(out->role, sizeof(out->role), "%s", role);
    out->login_time = time(NULL);
    out->last_activity = out->login_time;
    out->login_log_id = login_log_id;
    out->active = true;
    return true;
}

void session_logout(WmsDb *db, Session *s) {
    if (!s->active) return;
    if (s->login_log_id > 0) db_log_logout(db, s->login_log_id);
    memset(s, 0, sizeof(*s));
    s->active = false;
}

void session_touch(Session *s) {
    if (s->active) s->last_activity = time(NULL);
}

bool session_is_locked(const Session *s, int timeout_s) {
    if (!s->active || timeout_s <= 0) return false;
    return (time(NULL) - s->last_activity) > timeout_s;
}

/* Central role gate. Add new actions here as Phase 1 features (purchase
 * order approval, history deletion, etc.) need them -- everything else
 * calls this instead of re-implementing role checks. */
bool session_can(const Session *s, const char *action) {
    if (!s->active) return false;

    if (strcmp(s->role, "admin") == 0) return true; /* admin can do everything */

    if (strcmp(action, "product.delete") == 0)
        return true; /* any logged-in role can delete products */

    if (strcmp(action, "history.delete") == 0)
        return false; /* spec: admin-only */

    if (strcmp(action, "movement.post") == 0)
        return true; /* all roles can post movements */

    if (strcmp(action, "po.approve") == 0)
        return strcmp(s->role, "manager") == 0;

    if (strcmp(action, "category.delete") == 0)
        return strcmp(s->role, "manager") == 0;

    if (strcmp(action, "product.edit") == 0)
        return true; /* all roles can edit product details */

    /* Unknown action: fail closed. */
    return false;
}
