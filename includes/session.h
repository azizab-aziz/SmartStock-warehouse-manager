#ifndef SESSION_H
#define SESSION_H

#include <time.h>
#include <stdbool.h>
#include "db.h"

typedef struct {
    int    user_id;
    char   username[64];
    char   role[16];          /* "admin" | "manager" | "operateur" */
    time_t login_time;
    time_t last_activity;
    int    login_log_id;
    bool   active;            /* false = no one logged in */
} Session;

/* Verifies credentials and, on success, fills *out and opens a login_log
 * row. On failure returns false and writes a user-facing message (never
 * revealing whether the username or the password was wrong). */
bool session_login(WmsDb *db, const char *username, const char *password,
                    Session *out, char *err_out, size_t err_len);

/* Closes the login_log row and clears the session. */
void session_logout(WmsDb *db, Session *s);

/* Call on any detected user input (keypress, click) to reset the
 * inactivity clock. */
void session_touch(Session *s);

/* True once (now - last_activity) exceeds timeout_s. */
bool session_is_locked(const Session *s, int timeout_s);

/* Role gate: does this session's role permit `action`? Central place so
 * every write function and every GUI button checks the same rule. */
bool session_can(const Session *s, const char *action);

#endif
