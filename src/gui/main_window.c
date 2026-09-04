#include "gui.h"
#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <direct.h>   /* _mkdir - MinGW/Windows */
#include <dirent.h>   /* opendir/readdir - listing backups/ */
#include <time.h>     /* timestamped backup filenames */


#define _GNU_SOURCE
#define PAGE_SIZE 10
#define SCREEN_W  1024
#define SCREEN_H  768
#define COLOR_NAVY_DARK   (Color){ 15, 23, 42, 255 }     /* sidebar/header bg */
#define COLOR_NAVY_MID    (Color){ 30, 41, 59, 255 }      /* panel accents */
#define COLOR_ACCENT_BLUE (Color){ 37, 99, 235, 255 }     /* primary buttons */
#define COLOR_ACCENT_TEAL (Color){ 16, 185, 129, 255 }    /* success/positive */
#define COLOR_ACCENT_RED  (Color){ 220, 38, 38, 255 }     /* errors/alerts */
#define COLOR_ACCENT_ORANGE (Color){ 217, 119, 6, 255 }   /* low-stock warning (distinct from out-of-stock red) */

/* Fixed unit choices - stored verbatim in products.unit (TEXT, 16 char
 * limit). A closed list rather than free text keeps stats/exports
 * consistent (no "pcs" vs "piece" vs "Piece" fragmentation). */
static const char *UNIT_OPTIONS[] = { "piece", "boite", "kg", "litre" };
#define UNIT_OPTION_COUNT ((int)(sizeof(UNIT_OPTIONS) / sizeof(UNIT_OPTIONS[0])))
#define COLOR_BG_LIGHT    (Color){ 248, 250, 252, 255 }   /* main content bg */
#define COLOR_TEXT_MUTED  (Color){ 100, 116, 139, 255 }   /* secondary text */
#define COLOR_BORDER      (Color){ 226, 232, 240, 255 }   /* card borders */

#include "session.h"

typedef enum { SCREEN_SPLASH, SCREEN_LOGIN, SCREEN_CATEGORIES, SCREEN_MAIN, SCREEN_STATS, SCREEN_CATEGORY_STATS, SCREEN_AUDIT_LOG, SCREEN_ARCHIVED_PRODUCTS, SCREEN_STOCK_ALERTS, SCREEN_BACKUP, SCREEN_SUPPLIERS, SCREEN_PURCHASE_ORDERS, SCREEN_PO_DETAIL } Screen;
static Screen  g_screen = SCREEN_SPLASH;
static int     g_active_category_id = 0;      /* 0 = "no filter" (unused once categories screen exists) */
static char    g_active_category_name[64] = "";
static int     g_active_po_id = 0;
static char    g_active_po_number[32] = "";
static float   g_splashTimer = 0.0f;
static Session g_session = {0};
static Font g_appFont;
static Font g_appFontBold;
static Texture2D g_logoLockupLight;
static Texture2D g_logoLockupDark;
static Texture2D g_logoMark;
static bool g_logoLoaded = false;

static void AppText(const char *text, int x, int y, int size, Color color);
static void AppTextBold(const char *text, int x, int y, int size, Color color);

static char login_username[64] = "";
static char login_password[64] = "";
static bool username_edit = false;
static bool password_edit = false;
static char login_error[128] = "";
static float error_fade = 0.0f;

/* --- Signup (create account) state --- */
static bool signup_mode = false;
static char signup_username[64] = "";
static char signup_password[64] = "";
static char signup_confirm[64]  = "";
static bool su_username_edit = false, su_password_edit = false, su_confirm_edit = false;

/* Generic Tab/↑↓/Enter navigation for a group of raygui text boxes.
 * edit_flags: pointers to each field's edit-mode bool, in visual order
 *             (same order the fields are drawn top-to-bottom).
 * count: how many fields in the group.
 * Returns true only when Enter is pressed while focus is on the LAST
 * field - the "auto-submit" trigger requested by the spec. */
typedef struct { bool moved; bool submit; } NavResult;

static NavResult nav_handle_focus(bool **edit_flags, int count) {
    NavResult r = { false, false };
    int current = 0;
    for (int i = 0; i < count; i++) {
        if (*edit_flags[i]) { current = i; break; }
    }

    bool move_forward = IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_DOWN) ||
                         (IsKeyPressed(KEY_ENTER) && current < count - 1);

    if (move_forward) {
        for (int i = 0; i < count; i++) *edit_flags[i] = false;
        current = (current + 1) % count;
        *edit_flags[current] = true;
        r.moved = true;
        return r;
    }

    if (IsKeyPressed(KEY_UP)) {
        for (int i = 0; i < count; i++) *edit_flags[i] = false;
        current = (current - 1 + count) % count;
        *edit_flags[current] = true;
        r.moved = true;
        return r;
    }

        r.submit = (current == count - 1) && IsKeyPressed(KEY_ENTER);
    return r;

}

/* Wraps a left-flowing button row to a new line if the next button of
 * `width` wouldn't fit in the current window - keeps every button
 * reachable at any window size instead of running off the right edge.
 * Shared by the categories-screen toolbar and the main product toolbar. */
static void toolbar_wrap(int *tx, int *toolbar_y, int width, int sx, int sh) {
    if (*tx + width > GetScreenWidth() - 20) {
        *tx = sx;
        *toolbar_y += sh + 10;
    }
}

static void draw_splash_screen(void) {
    ClearBackground(COLOR_NAVY_DARK);

    float t = g_splashTimer;
    const float total = 1.8f; /* total splash duration in seconds */

    /* Logo scales up and fades in during the first 0.6s, holds, then the
       whole screen fades out during the last 0.4s. */
    float scale_progress = fminf(t / 0.5f, 1.0f);
    float ease = 1.0f - (1.0f - scale_progress) * (1.0f - scale_progress); /* ease-out */
    float logo_scale = 0.7f + 0.3f * ease;

    float fade_out_start = total - 0.4f;
    float alpha = (t > fade_out_start) ? fmaxf(0.0f, 1.0f - (t - fade_out_start) / 0.4f) : 1.0f;

    float logo_size = 140.0f * logo_scale;
    float lx = GetScreenWidth()/2 - logo_size/2;
    float ly = GetScreenHeight()/2 - logo_size/2 - 30;

    Color tint = { 255, 255, 255, (unsigned char)(255 * alpha) };
    DrawTextureEx(g_logoMark, (Vector2){ lx, ly }, 0, logo_size / g_logoMark.height, tint);

    const char *title = "SmartStock";
    Vector2 tsize = MeasureTextEx(g_appFontBold, title, 30, 1);
    Color title_color = { 255, 255, 255, (unsigned char)(255 * fminf(alpha, ease)) };
    AppTextBold(title, GetScreenWidth()/2 - tsize.x/2, ly + logo_size + 20, 30, title_color);

    g_splashTimer += GetFrameTime();
    if (g_splashTimer >= total) {
        g_screen = SCREEN_LOGIN;
    }
}

static void draw_login_screen(WmsDb *db) {
    ClearBackground(COLOR_NAVY_DARK);
    int panel_w = 380, panel_h = signup_mode ? 400 : 340;
    int px = (GetScreenWidth() - panel_w) / 2, py = (GetScreenHeight() - panel_h) / 2;

            if (!signup_mode) {
        DrawRectangleRounded((Rectangle){ px, py, panel_w, panel_h }, 0.06f, 8, WHITE);
        DrawRectangleRoundedLines((Rectangle){ px, py, panel_w, panel_h }, 0.06f, 8, 1, COLOR_BORDER);

        if (g_logoLoaded) {
            float logo_h = 44, logo_w = g_logoLockupLight.width * (logo_h / g_logoLockupLight.height);
            DrawTextureEx(g_logoLockupLight, (Vector2){ px + 24, py + 20 }, 0, logo_h / g_logoLockupLight.height, WHITE);
            AppText("Connexion a votre espace", px + 24, py + 20 + logo_h + 8, 14, COLOR_TEXT_MUTED);
        } else {
            AppTextBold("SmartStock", px + 24, py + 24, 22, (Color){30,41,59,255});
            AppText("Connexion a votre espace", px + 24, py + 54, 14, COLOR_TEXT_MUTED);
        }

        if (!username_edit && !password_edit) username_edit = true; /* default focus */
        bool *login_fields[2] = { &username_edit, &password_edit };
        NavResult login_nav = nav_handle_focus(login_fields, 2);

        GuiLabel((Rectangle){ px + 24, py + 92, 100, 20 }, "Utilisateur");
        if (GuiTextBox((Rectangle){ px + 24, py + 114, panel_w - 48, 36 },
                        login_username, sizeof(login_username), username_edit) && !login_nav.moved) {
            username_edit = !username_edit;
            if (username_edit) password_edit = false;
        }

        GuiLabel((Rectangle){ px + 24, py + 160, 100, 20 }, "Mot de passe");
        if (GuiTextBox((Rectangle){ px + 24, py + 182, panel_w - 48, 36 },
                        login_password, sizeof(login_password), password_edit) && !login_nav.moved) {
            password_edit = !password_edit;
            if (password_edit) username_edit = false;
        }

        Rectangle submit_rect = { px + 24, py + 232, panel_w - 48, 40 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), submit_rect);
        DrawRectangleRounded(submit_rect, 0.2f, 6, hover ? (Color){29,78,216,255} : COLOR_ACCENT_BLUE);
        Vector2 tsize = MeasureTextEx(g_appFont, "Se connecter", 16, 1);
        AppText("Se connecter", submit_rect.x + submit_rect.width/2 - tsize.x/2,
                 submit_rect.y + 12, 16, WHITE);
        bool submit = (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || login_nav.submit;

        if (submit) {
            char err[128];
                       if (session_login(db, login_username, login_password, &g_session, err, sizeof(err))) {
                memset(login_password, 0, sizeof(login_password));
                g_screen = SCREEN_CATEGORIES;
            } else {
                snprintf(login_error, sizeof(login_error), "%s", err);
                error_fade = 1.0f;
            }
        }
        if (error_fade > 0.0f) {
            Color c = COLOR_ACCENT_RED;
            c.a = (unsigned char)(error_fade * 255);
            AppText(login_error, px + 24, py + 282, 13, c);
            error_fade -= GetFrameTime() * 0.5f;
        }

        /* Link sits BELOW the button with real spacing - no overlap. */
        const char *link_text = "Pas encore de compte ? Creer un compte";
        Vector2 link_size = MeasureTextEx(g_appFont, link_text, 13, 1);
        Rectangle link_rect = { px + panel_w/2 - link_size.x/2 - 6, py + panel_h - 40, link_size.x + 12, 24 };
        bool link_hover = CheckCollisionPointRec(GetMousePosition(), link_rect);
        AppText(link_text, link_rect.x + 6, link_rect.y + 5, 13,
                 link_hover ? COLOR_ACCENT_BLUE : COLOR_TEXT_MUTED);
        if (link_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            signup_mode = true;
            login_error[0] = '\0';
            signup_username[0] = signup_password[0] = signup_confirm[0] = '\0';
        }


    } else {
        /* ---- SIGNUP FORM ---- */
        GuiPanel((Rectangle){ px, py, panel_w, panel_h }, "SmartStock -- Creer un compte");
                if (!su_username_edit && !su_password_edit && !su_confirm_edit) su_username_edit = true;
        bool *signup_fields[3] = { &su_username_edit, &su_password_edit, &su_confirm_edit };
        NavResult signup_nav = nav_handle_focus(signup_fields, 3);

        GuiLabel((Rectangle){ px + 24, py + 50, 140, 24 }, "Utilisateur");
        if (GuiTextBox((Rectangle){ px + 24, py + 76, panel_w - 48, 32 },
                        signup_username, sizeof(signup_username), su_username_edit) && !signup_nav.moved) {
            su_username_edit = !su_username_edit;
            if (su_username_edit) { su_password_edit = su_confirm_edit = false; }
        }

        GuiLabel((Rectangle){ px + 24, py + 116, 140, 24 }, "Mot de passe (8+ caracteres)");
        if (GuiTextBox((Rectangle){ px + 24, py + 142, panel_w - 48, 32 },
                        signup_password, sizeof(signup_password), su_password_edit) && !signup_nav.moved) {
            su_password_edit = !su_password_edit;
            if (su_password_edit) { su_username_edit = su_confirm_edit = false; }
        }

        GuiLabel((Rectangle){ px + 24, py + 182, 140, 24 }, "Confirmer le mot de passe");
        if (GuiTextBox((Rectangle){ px + 24, py + 208, panel_w - 48, 32 },
                        signup_confirm, sizeof(signup_confirm), su_confirm_edit) && !signup_nav.moved) {
            su_confirm_edit = !su_confirm_edit;
            if (su_confirm_edit) { su_username_edit = su_password_edit = false; }
        }

        if (GuiButton((Rectangle){ px + 24, py + 256, panel_w - 48, 36 }, "S'inscrire") || signup_nav.submit) {
            if (strlen(signup_username) < 3) {
                snprintf(login_error, sizeof(login_error), "Nom d'utilisateur trop court");
                error_fade = 1.0f;
            } else if (strlen(signup_password) < 8) {
                snprintf(login_error, sizeof(login_error), "Mot de passe trop court (8 min)");
                error_fade = 1.0f;
            } else if (strcmp(signup_password, signup_confirm) != 0) {
                snprintf(login_error, sizeof(login_error), "Les mots de passe ne correspondent pas");
                error_fade = 1.0f;
            } else {
                int existing = 0;
                db_count_users(db, &existing);
                const char *role = (existing == 0) ? "admin" : "operateur";

                char err[256];
                if (db_create_user(db, signup_username, signup_password, role, err, sizeof(err))) {
                    /* auto-login right after signup */
                    char login_err[128];
                    session_login(db, signup_username, signup_password, &g_session, login_err, sizeof(login_err));
                    memset(signup_password, 0, sizeof(signup_password));
                    memset(signup_confirm, 0, sizeof(signup_confirm));
                    signup_mode = false;
                    g_screen = SCREEN_CATEGORIES;
                } else {
                    snprintf(login_error, sizeof(login_error), "%s", err);
                    error_fade = 1.0f;
                }
            }
        }

        if (error_fade > 0.0f) {
            Color c = RED;
            c.a = (unsigned char)(error_fade * 255);
            DrawText(login_error, px + 24, py + 300, 14, c);
            error_fade -= GetFrameTime() * 0.5f;
        }

        if (GuiButton((Rectangle){ px + 24, py + panel_h - 34, panel_w - 48, 26 }, "Retour a la connexion")) {
            signup_mode = false;
            login_error[0] = '\0';
        }
    }
}


typedef enum { PANEL_NONE, PANEL_ADD_PRODUCT, PANEL_MOVEMENT,
               PANEL_EDIT_PRODUCT, PANEL_CONFIRM_DELETE_PRODUCT,
               PANEL_MANAGE_CATEGORIES, PANEL_CONFIRM_DELETE_CATEGORY,
               PANEL_MANAGE_USERS, PANEL_RENAME_CATEGORY,
               PANEL_MOVEMENT_HISTORY } ActivePanel;

/* Small helper: a message that fades out after ~1.5s, per the spec
 * ("Messages de succes avec fade-out apres chaque action"). */
typedef struct {
    char text[128];
    float timer;      /* seconds remaining */
    bool  is_error;
} Toast;

static void toast_show(Toast *t, const char *msg, bool is_error) {
    snprintf(t->text, sizeof t->text, "%s", msg);
    t->timer = 1.8f;
    t->is_error = is_error;
}

#define EXPORT_CSV_PATH  "exports/export.csv"
#define EXPORT_XLSX_PATH "exports/rapport_stock.xlsx"
#define EXPORT_PY_SCRIPT "python_scripts/export_report.py"

/* Writes the CSV, shells out to the Python/openpyxl script, and reports
 * success/failure via toast. category_id <= 0 exports every category. */
static void run_excel_export(int category_id, const char *sheet_name, Toast *toast) {
    _mkdir("exports"); /* ignored if it already exists */

    char err[256];
    if (!inv_export_csv(category_id, EXPORT_CSV_PATH, err, sizeof err)) {
        toast_show(toast, err, true);
        return;
    }

    /* Full-catalog export always writes to the same fixed report;
     * a single-category export gets its own file (by category id, not
     * name, to avoid accented/space filename issues) so the two never
     * clobber each other. */
    char xlsx_path[128];
    if (category_id > 0)
        snprintf(xlsx_path, sizeof xlsx_path, "exports/rapport_stock_cat%d.xlsx", category_id);
    else
        snprintf(xlsx_path, sizeof xlsx_path, "%s", EXPORT_XLSX_PATH);

    /* Redirect the python script's own stdout/stderr to a log file, since
       this app has no console - "type exports\export_log.txt" afterwards
       shows exactly why it failed (missing python, missing script,
       openpyxl install error, etc). */
    char cmd[900];
    snprintf(cmd, sizeof cmd,
             "python \"%s\" \"%s\" \"%s\" \"%s\" > \"exports\\export_log.txt\" 2>&1",
             EXPORT_PY_SCRIPT, EXPORT_CSV_PATH, xlsx_path, sheet_name);
    int rc = system(cmd);

    FILE *dbg = fopen("exports/export_debug.txt", "w");
    if (dbg) {
        fprintf(dbg, "command: %s\n", cmd);
        fprintf(dbg, "system() return code: %d\n", rc);
        fclose(dbg);
    }

    if (rc == 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "Rapport Excel genere (%s)", xlsx_path);
        toast_show(toast, msg, false);
    } else {
        toast_show(toast, "Erreur - voir exports/export_log.txt", true);
    }
}

#define EXPORT_INFO_CSV_PATH "exports/fiche_info.csv"
#define EXPORT_MOV_CSV_PATH  "exports/fiche_mouvements.csv"
#define EXPORT_PDF_SCRIPT    "python_scripts/product_sheet.py"

/* Writes the two CSVs (product info + movement history), shells out to
 * the Python/reportlab script, and reports success/failure via toast. */
static void run_product_sheet_export(Product *p, Toast *toast) {
    if (!p) {
        toast_show(toast, "Selectionnez un produit d'abord", true);
        return;
    }
    _mkdir("exports");

    char err[256];
    if (!inv_export_product_info_csv(p->id, EXPORT_INFO_CSV_PATH, err, sizeof err)) {
        toast_show(toast, err, true);
        return;
    }
    if (!inv_export_movements_csv(p->id, EXPORT_MOV_CSV_PATH, 200, err, sizeof err)) {
        toast_show(toast, err, true);
        return;
    }

    char pdf_path[128];
    snprintf(pdf_path, sizeof pdf_path, "exports/fiche_%s.pdf", p->sku);

    char cmd[900];
    snprintf(cmd, sizeof cmd,
             "python \"%s\" \"%s\" \"%s\" \"%s\" > \"exports\\sheet_log.txt\" 2>&1",
             EXPORT_PDF_SCRIPT, EXPORT_INFO_CSV_PATH, EXPORT_MOV_CSV_PATH, pdf_path);
    int rc = system(cmd);

    FILE *dbg = fopen("exports/sheet_debug.txt", "w");
    if (dbg) {
        fprintf(dbg, "command: %s\n", cmd);
        fprintf(dbg, "system() return code: %d\n", rc);
        fclose(dbg);
    }

    if (rc == 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "Fiche PDF generee (%s)", pdf_path);
        toast_show(toast, msg, false);
    } else {
        toast_show(toast, "Erreur - voir exports/sheet_log.txt", true);
    }
}

/* Finds a product by its stable database id, searching the full in-memory
 * array directly - independent of whatever filter/category view is
 * currently on screen. Returns NULL if not found (e.g. it was deleted). */
static Product *find_product_by_id(Product *arr, int count, int id) {
    for (int i = 0; i < count; i++)
        if (arr[i].id == id) return &arr[i];
    return NULL;
}

/* Portable case-insensitive substring search (strcasestr is a glibc/BSD
 * extension not reliably available on MinGW). Returns true if `needle`
 * appears anywhere in `haystack`, ignoring case. */
static bool ci_str_contains(const char *haystack, const char *needle) {
    if (!needle[0]) return true;
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = haystack[i+j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

/* True only if `s` is entirely digits, with at most one optional leading
 * '-'. Rejects empty strings, "-" alone, and anything with letters -
 * unlike atoi(), which silently returns 0 for garbage input. */
static bool str_is_integer(const char *s) {
    if (!s || !s[0]) return false;
    int i = (s[0] == '-') ? 1 : 0;
    if (!s[i]) return false;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* Same idea for decimals: digits, at most one '.', optional leading '-'. */
static bool str_is_decimal(const char *s) {
    if (!s || !s[0]) return false;
    int i = (s[0] == '-') ? 1 : 0;
    if (!s[i]) return false;
    bool seen_digit = false, seen_dot = false;
    for (; s[i]; i++) {
        if (s[i] == '.') {
            if (seen_dot) return false;
            seen_dot = true;
        } else if (s[i] >= '0' && s[i] <= '9') {
            seen_digit = true;
        } else {
            return false;
        }
    }
    return seen_digit;
}

/* Shared validation for the add-product and edit-product forms.
 * sku:     pass NULL to skip the SKU checks entirely (edit form has no
 *          SKU field - it's immutable after creation).
 * qty_str: pass NULL to skip (edit form has no initial-quantity field);
 *          when non-NULL, empty string is allowed (blank = 0).
 * Returns true if everything is valid; otherwise fills err_out. */
static bool validate_product_fields(const char *sku, const char *name,
                                     const char *price_str, const char *threshold_str,
                                     const char *qty_str,
                                     char *err_out, size_t err_len) {
    if (sku && sku[0] == '\0') {
        snprintf(err_out, err_len, "Le SKU est obligatoire"); return false;
    }
    if (!name || name[0] == '\0') {
        snprintf(err_out, err_len, "Le nom est obligatoire"); return false;
    }
    if (!str_is_decimal(price_str)) {
        snprintf(err_out, err_len, "Prix unitaire invalide"); return false;
    }
    if (atof(price_str) < 0) {
        snprintf(err_out, err_len, "Le prix ne peut pas etre negatif"); return false;
    }
    if (!str_is_integer(threshold_str)) {
        snprintf(err_out, err_len, "Seuil d'alerte invalide"); return false;
    }
    if (atoi(threshold_str) < 0) {
        snprintf(err_out, err_len, "Le seuil d'alerte ne peut pas etre negatif"); return false;
    }
    if (qty_str && qty_str[0] != '\0') {
        if (!str_is_integer(qty_str)) {
            snprintf(err_out, err_len, "Quantite initiale invalide"); return false;
        }
        if (atoi(qty_str) < 0) {
            snprintf(err_out, err_len, "La quantite initiale ne peut pas etre negative"); return false;
        }
    }
    if (sku && sku[0] && inv_find_by_sku(sku) != NULL) {
        snprintf(err_out, err_len, "Ce SKU existe deja"); return false;
    }
    return true;
}

static void draw_categories_screen(WmsDb *db, Category *all_categories, int *total_categories,
                                    char *cat_search, bool *cat_search_edit,
                                    int *cat_action_id, char *cat_rename_input, bool *cat_rename_edit,
                                    bool *cat_renaming, ActivePanel *cat_screen_panel, Toast *toast,
                                    int *mgmt_user_ids, char mgmt_user_names[][64], char mgmt_user_roles[][16],
                                    int *mgmt_user_count,
                                    char *global_search, bool *global_search_edit) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Catalogue par categorie", 20, 46, 14, (Color){180,190,210,255});

    char user_label[96];
    snprintf(user_label, sizeof user_label, "%s (%s)", g_session.username, g_session.role);
    Vector2 user_size = MeasureTextEx(g_appFont, user_label, 14, 1);
    Rectangle logout_rect = { GetScreenWidth() - 130, 16, 110, 32 };
    AppText(user_label, logout_rect.x - 16 - user_size.x, 24, 14, (Color){180,190,210,255});
    bool logout_hover = CheckCollisionPointRec(GetMousePosition(), logout_rect);
    DrawRectangleRounded(logout_rect, 0.25f, 6, logout_hover ? (Color){153,27,27,255} : (Color){71,85,105,255});
    Vector2 logout_tsize = MeasureTextEx(g_appFont, "Deconnexion", 14, 1);
    AppText("Deconnexion", logout_rect.x + logout_rect.width/2 - logout_tsize.x/2, logout_rect.y + 8, 14, WHITE);
    if (logout_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        session_logout(db, &g_session);
        g_screen = SCREEN_LOGIN;
    }

    bool modal_active = (*cat_screen_panel != PANEL_NONE);

       /* Search bar */
    int sx = 20, sy = 94, sw = 400, sh = 32;
    if (GuiTextBox((Rectangle){ sx, sy, sw, sh }, cat_search, 128, *cat_search_edit) && !modal_active)
        *cat_search_edit = !*cat_search_edit;

    /* Global product search - separate from the category-name filter
    above. Searches every product regardless of category, and shows
    which category each result belongs to. */
    int gsx = sx, gsy = sy + sh + 34;
    GuiLabel((Rectangle){ gsx, gsy - 20, 300, 18 }, "Rechercher un produit (toutes categories)");
    if (GuiTextBox((Rectangle){ gsx, gsy, sw, sh }, global_search, 128, *global_search_edit) && !modal_active)
        *global_search_edit = !*global_search_edit;

    /* Action buttons - own row below both search boxes, wrapping to a
       second row instead of overlapping the search bars or each other
       when the window is narrower than the full toolbar width. */
    int cat_toolbar_y = gsy + sh + 22;
    int cat_tx = sx;
    int cat_btn_gap = 10;

    if (session_can(&g_session, "user.manage")) {
        toolbar_wrap(&cat_tx, &cat_toolbar_y, 170, sx, sh);
        if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 170, sh }, "Gerer utilisateurs") && !modal_active) {
            *cat_screen_panel = PANEL_MANAGE_USERS;
        }
        cat_tx += 170 + cat_btn_gap;
    }

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 190, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 190, sh }, "+ Ajouter categorie") && !modal_active) {
        *cat_screen_panel = PANEL_ADD_PRODUCT; /* reused as a generic "add category" dialog trigger below */
        cat_rename_input[0] = '\0';
        *cat_rename_edit = true;
        *cat_renaming = false;
        *cat_action_id = 0;
    }
    cat_tx += 190 + cat_btn_gap;

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 150, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 150, sh }, "Exporter Excel") && !modal_active) {
        run_excel_export(0, "Tout le stock", toast);
    }
    cat_tx += 150 + cat_btn_gap;

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 150, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 150, sh }, "Journal d'audit") && !modal_active) {
        g_screen = SCREEN_AUDIT_LOG;
    }
    cat_tx += 150 + cat_btn_gap;

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 170, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 170, sh }, "Produits archives") && !modal_active) {
        g_screen = SCREEN_ARCHIVED_PRODUCTS;
    }
    cat_tx += 170 + cat_btn_gap;

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 130, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 130, sh }, "Alertes stock") && !modal_active) {
        g_screen = SCREEN_STOCK_ALERTS;
    }
    cat_tx += 130 + cat_btn_gap;

        if (session_can(&g_session, "user.manage")) {
        toolbar_wrap(&cat_tx, &cat_toolbar_y, 130, sx, sh);
        if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 130, sh }, "Sauvegarde") && !modal_active) {
            g_screen = SCREEN_BACKUP;
        }
        cat_tx += 130 + cat_btn_gap;
    }

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 140, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 140, sh }, "Fournisseurs") && !modal_active) {
        g_screen = SCREEN_SUPPLIERS;
    }
    cat_tx += 140 + cat_btn_gap;

    toolbar_wrap(&cat_tx, &cat_toolbar_y, 140, sx, sh);
    if (GuiButton((Rectangle){ cat_tx, cat_toolbar_y, 140, sh }, "Commandes") && !modal_active) {
        g_screen = SCREEN_PURCHASE_ORDERS;
    }

    /* Alphabetical list (already sorted by db_list_categories via
       ORDER BY name COLLATE NOCASE) - filtered live by the search box. */
    int row_y = cat_toolbar_y + sh + 30; /* dynamic - accounts for the toolbar wrapping to 2 rows */
    int shown = 0;

    /* If a global product search is active, show matching products
       instead of the category list - each result labelled with its
       category so you always know where it lives. */
    if (global_search[0] != '\0') {
        Product *all_products_ref;
        int total_products_ref = inv_all_products(&all_products_ref);
        Product *matches[64];
        int match_count = inv_search(global_search, matches, 64);

        AppText(TextFormat("%d resultat(s) pour \"%s\"", match_count, global_search),
                 gsx, row_y - 20, 13, COLOR_TEXT_MUTED);

        if (match_count == 0) {
            AppText("Aucun produit ne correspond a cette recherche.", sx, row_y, 14, COLOR_TEXT_MUTED);
        }

        for (int mi = 0; mi < match_count; mi++) {
            Product *p = matches[mi];
            Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 44 };
            bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect) && !modal_active;
            if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
            DrawLine((int)row_rect.x, row_y + 40, (int)(row_rect.x + row_rect.width), row_y + 40, COLOR_BORDER);

            AppTextBold(p->name, sx + 8, row_y + 4, 15, (Color){30,41,59,255});
            AppText(p->sku, sx + 8, row_y + 24, 12, COLOR_TEXT_MUTED);

            /* Category label - the whole point of this feature. */
            const char *cat_name = "Sans categorie";
            for (int ci2 = 0; ci2 < *total_categories; ci2++)
                if (all_categories[ci2].id == p->category_id) { cat_name = all_categories[ci2].name; break; }
            Vector2 cat_size = MeasureTextEx(g_appFont, cat_name, 13, 1);
            AppText(cat_name, GetScreenWidth() - sx - cat_size.x - 8, row_y + 12, 13, COLOR_ACCENT_BLUE);

            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_active_category_id = p->category_id;
                for (int ci2 = 0; ci2 < *total_categories; ci2++)
                    if (all_categories[ci2].id == p->category_id)
                        snprintf(g_active_category_name, sizeof g_active_category_name, "%s", all_categories[ci2].name);
                g_screen = SCREEN_MAIN;
            }
            row_y += 48;
        }
    }else{
        for (int ci = 0; ci < *total_categories; ci++) {
               if (cat_search[0] && !ci_str_contains(all_categories[ci].name, cat_search)) continue;
        shown++;

        Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 44 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect) && !modal_active;
        if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
        DrawLine((int)row_rect.x, row_y + 40, (int)(row_rect.x + row_rect.width), row_y + 40, COLOR_BORDER);

        AppTextBold(all_categories[ci].name, sx + 8, row_y + 8, 16, (Color){30,41,59,255});

        Rectangle modify_btn = { GetScreenWidth() - 260, row_y + 6, 90, 28 };
        Rectangle delete_btn = { GetScreenWidth() - 160, row_y + 6, 90, 28 };

        bool mod_hover = CheckCollisionPointRec(GetMousePosition(), modify_btn) && !modal_active;
        DrawRectangleLinesEx(modify_btn, 1, COLOR_BORDER);
        if (mod_hover) DrawRectangleRec(modify_btn, (Color){239,246,255,255});
        AppText("Modifier", modify_btn.x + 14, modify_btn.y + 6, 13, (Color){30,41,59,255});
        if (mod_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            *cat_action_id = all_categories[ci].id;
            snprintf(cat_rename_input, 64, "%s", all_categories[ci].name);
            *cat_renaming = true;
            *cat_rename_edit = true;
            *cat_screen_panel = PANEL_EDIT_PRODUCT; /* reused as "rename category" dialog trigger */
        }

        bool del_hover = CheckCollisionPointRec(GetMousePosition(), delete_btn) && !modal_active;
        DrawRectangleLinesEx(delete_btn, 1, COLOR_BORDER);
        if (del_hover) DrawRectangleRec(delete_btn, (Color){254,242,242,255});
        AppText("Supprimer", delete_btn.x + 10, delete_btn.y + 6, 13, (Color){185,28,28,255});
        if (del_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            *cat_action_id = all_categories[ci].id;
            *cat_screen_panel = PANEL_CONFIRM_DELETE_CATEGORY;
        }

        /* Clicking the row itself (not the two buttons) enters the category. */
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !CheckCollisionPointRec(GetMousePosition(), modify_btn) &&
            !CheckCollisionPointRec(GetMousePosition(), delete_btn)) {
            g_active_category_id = all_categories[ci].id;
            snprintf(g_active_category_name, sizeof g_active_category_name, "%s", all_categories[ci].name);
            g_screen = SCREEN_MAIN;
        }

        row_y += 48;
    }


    if (shown == 0) {
        AppText(cat_search[0] ? "Aucune categorie ne correspond a la recherche."
                              : "Aucune categorie pour le moment. Cliquez sur \"+ Ajouter categorie\".",
                 sx, row_y, 14, COLOR_TEXT_MUTED);
    }
 }

    /* ---- Add category dialog (reusing PANEL_ADD_PRODUCT as the trigger) ---- */
    if (*cat_screen_panel == PANEL_ADD_PRODUCT && !*cat_renaming) {
        Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 80, 400, 160 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Nouvelle categorie");
        float bx = box.x + 20, by = box.y + 40;
        GuiTextBox((Rectangle){ bx, by, box.width - 40, 30 }, cat_rename_input, 64, *cat_rename_edit);

        by += 46;
        bool confirm = GuiButton((Rectangle){ bx, by, 150, 32 }, "Ajouter") ||
                       (*cat_rename_edit && IsKeyPressed(KEY_ENTER));
        if (confirm) {
            if (cat_rename_input[0] != '\0') {
                int new_id;
                if (db_find_or_create_category(db, cat_rename_input, &new_id)) {
                    inv_refresh_categories(db);
                    *total_categories = inv_get_categories(&all_categories);
                    toast_show(toast, "Categorie ajoutee", false);
                } else {
                    toast_show(toast, "Erreur lors de l'ajout", true);
                }
            }
            *cat_screen_panel = PANEL_NONE;
        }
        if (GuiButton((Rectangle){ bx + 160, by, 150, 32 }, "Annuler")) *cat_screen_panel = PANEL_NONE;
    }

    /* ---- Rename category dialog (reusing PANEL_EDIT_PRODUCT as the trigger) ---- */
    if (*cat_screen_panel == PANEL_EDIT_PRODUCT && *cat_renaming) {
        Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 80, 400, 160 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Renommer la categorie");
        float bx = box.x + 20, by = box.y + 40;
        GuiTextBox((Rectangle){ bx, by, box.width - 40, 30 }, cat_rename_input, 64, *cat_rename_edit);

        by += 46;
        bool confirm = GuiButton((Rectangle){ bx, by, 150, 32 }, "Enregistrer") ||
                       (*cat_rename_edit && IsKeyPressed(KEY_ENTER));
        if (confirm) {
            if (cat_rename_input[0] != '\0' &&
                db_update_category_name(db, *cat_action_id, cat_rename_input)) {
                inv_refresh_categories(db);
                *total_categories = inv_get_categories(&all_categories);
                toast_show(toast, "Categorie renommee", false);
            } else {
                toast_show(toast, "Erreur lors du renommage", true);
            }
            *cat_screen_panel = PANEL_NONE;
            *cat_renaming = false;
        }
        if (GuiButton((Rectangle){ bx + 160, by, 150, 32 }, "Annuler")) {
            *cat_screen_panel = PANEL_NONE;
            *cat_renaming = false;
        }
    }

    /* ---- Confirm delete (reuses your existing PANEL_CONFIRM_DELETE_CATEGORY UI logic) ---- */
    if (*cat_screen_panel == PANEL_CONFIRM_DELETE_CATEGORY) {
        Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 90, 400, 180 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Confirmer la suppression");
        float bx = box.x + 20, by = box.y + 40;
        AppText("Supprimer cette categorie ?", bx, by, 14, (Color){30,41,59,255});

        by += 40;
        if (GuiButton((Rectangle){ bx, by, 160, 34 }, "Oui, supprimer")) {
            char err[256];
            if (inv_delete_category(*cat_action_id, &g_session, err, sizeof err)) {
                *total_categories = inv_get_categories(&all_categories);
                toast_show(toast, "Categorie supprimee", false);
            } else {
                toast_show(toast, err, true);
            }
            *cat_screen_panel = PANEL_NONE;
        }
        if (GuiButton((Rectangle){ bx + 180, by, 160, 34 }, "Annuler")) *cat_screen_panel = PANEL_NONE;
    }


    /* ---- Manage users panel (was missing - this was the actual bug) ---- */
    if (*cat_screen_panel == PANEL_MANAGE_USERS) {
        float list_h = 36.0f * (*mgmt_user_count > 0 ? *mgmt_user_count : 1) + 90;
        Rectangle box = { GetScreenWidth()/2 - 260, GetScreenHeight()/2 - list_h/2, 520, list_h };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Gerer les utilisateurs");

        float bx = box.x + 20, by = box.y + 40;
        const char *roles[3] = { "operateur", "manager", "admin" };

        for (int ui = 0; ui < *mgmt_user_count; ui++) {
            AppText(mgmt_user_names[ui], bx, by + 6, 14, (Color){30,41,59,255});

            for (int ri = 0; ri < 3; ri++) {
                bool is_current = strcmp(mgmt_user_roles[ui], roles[ri]) == 0;
                Rectangle rbtn = { box.x + box.width - 300 + ri * 100, by, 90, 28 };
                bool hover = CheckCollisionPointRec(GetMousePosition(), rbtn);
                DrawRectangleRec(rbtn, is_current ? COLOR_ACCENT_BLUE :
                                         (hover ? (Color){239,246,255,255} : WHITE));
                DrawRectangleLinesEx(rbtn, 1, COLOR_BORDER);
                AppText(roles[ri], rbtn.x + 6, rbtn.y + 6, 12,
                         is_current ? WHITE : (Color){30,41,59,255});

                if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !is_current) {
                    if (db_update_user_role(db, mgmt_user_ids[ui], roles[ri])) {
                        snprintf(mgmt_user_roles[ui], 16, "%s", roles[ri]);
                        toast_show(toast, "Role mis a jour", false);
                    } else {
                        toast_show(toast, "Erreur lors de la mise a jour", true);
                    }
                }
            }
            by += 36;
        }

        if (GuiButton((Rectangle){ bx, box.y + box.height - 50, 130, 32 }, "Fermer"))
            *cat_screen_panel = PANEL_NONE;
    }
}

/* Horizontal bar chart: one row per category, bar length proportional to
 * `values[i]`. Shared by the two charts on the stats screen - only the
 * data array and value formatting (count vs. currency) differ. */
static void draw_hbar_chart(const char *title, Rectangle box, const char *const *labels,
                             double *values, int count, Color barColor, bool is_currency,
                             const char *empty_msg) {
    DrawRectangleRounded(box, 0.03f, 6, WHITE);
    DrawRectangleRoundedLines(box, 0.03f, 6, 1, COLOR_BORDER);
    AppTextBold(title, box.x + 16, box.y + 14, 15, (Color){30,41,59,255});

    if (count == 0) {
        AppText(empty_msg, box.x + 16, box.y + 50, 13, COLOR_TEXT_MUTED);
        return;
    }

    double max_val = 0;
    for (int i = 0; i < count; i++) if (values[i] > max_val) max_val = values[i];
    if (max_val <= 0) max_val = 1;

    float chart_top = box.y + 46;
    float chart_bottom = box.y + box.height - 14;
    float row_h = (chart_bottom - chart_top) / count;
    if (row_h > 34) row_h = 34;
    if (row_h < 16) row_h = 16;

    float label_w = 130;
    float bar_x = box.x + 16 + label_w;
    float bar_max_w = box.x + box.width - 16 - bar_x - 70; /* room for value text */
    if (bar_max_w < 20) bar_max_w = 20;

    for (int i = 0; i < count && (chart_top + i*row_h + row_h) <= chart_bottom; i++) {
        float y = chart_top + i * row_h;
        char label[40];
        snprintf(label, sizeof label, "%.20s", labels[i]);
        AppText(label, box.x + 16, y + row_h/2 - 7, 13, (Color){30,41,59,255});

        float bar_w = (float)(values[i] / max_val) * bar_max_w;
        Rectangle bar = { bar_x, y + row_h/2 - 7, bar_w, 14 };
        DrawRectangleRounded(bar, 0.3f, 4, barColor);

        char val_buf[32];
        if (is_currency) snprintf(val_buf, sizeof val_buf, "%.2f DT", values[i]);
        else             snprintf(val_buf, sizeof val_buf, "%d", (int)values[i]);
        AppText(val_buf, bar_x + bar_w + 8, y + row_h/2 - 7, 13, COLOR_TEXT_MUTED);
    }
}

static void draw_stats_screen(Category *all_categories, int total_categories,
                               Product *all_products, int total_products) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Statistiques", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    /* per-category stats (capped, same fixed-array style used elsewhere) */
    int cats = total_categories > 128 ? 128 : total_categories;
    double counts[128] = {0}, values[128] = {0};
    int total_qty = 0;
    double total_value = 0;

    for (int pi = 0; pi < total_products; pi++) {
        total_qty += all_products[pi].total_quantity;
        total_value += (double)all_products[pi].total_quantity * all_products[pi].unit_price;
        for (int ci = 0; ci < cats; ci++) {
            if (all_products[pi].category_id == all_categories[ci].id) {
                counts[ci] += 1;
                values[ci] += (double)all_products[pi].total_quantity * all_products[pi].unit_price;
                break;
            }
        }
    }

    char summary[160];
    snprintf(summary, sizeof summary, "%d categories | %d produits | %d unites en stock | %.2f DT de valeur totale",
             cats, total_products, total_qty, total_value);
    AppText(summary, 20, 90, 14, COLOR_TEXT_MUTED);

        int chart_top = 120, gap = 30;
    int chart_h = GetScreenHeight() - chart_top - 30;
    int chart_w = (GetScreenWidth() - 2*20 - gap) / 2;

    Rectangle left_area  = { 20, chart_top, chart_w, chart_h };
    Rectangle right_area = { 20 + chart_w + gap, chart_top, chart_w, chart_h };

    const char *cat_labels[128];
    for (int i = 0; i < cats; i++) cat_labels[i] = all_categories[i].name;

    draw_hbar_chart("Produits par categorie", left_area, cat_labels, counts, cats,
                     COLOR_ACCENT_BLUE, false, "Aucune categorie.");
    draw_hbar_chart("Valeur du stock par categorie", right_area, cat_labels, values, cats,
                     COLOR_ACCENT_TEAL, true, "Aucune categorie.");
}

static void draw_category_stats_screen(Product *all_products, int total_products) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    char header[96];
    snprintf(header, sizeof header, "Statistiques - %s", g_active_category_name);
    AppText(header, 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 110, 20, 90, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Retour", 14, 1);
    AppText("< Retour", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_MAIN;

    /* products belonging to the active category only (capped at 128) */
    Product *in_cat[128];
    int count = 0;
    for (int pi = 0; pi < total_products && count < 128; pi++)
        if (all_products[pi].category_id == g_active_category_id)
            in_cat[count++] = &all_products[pi];

    int total_qty = 0;
    double total_value = 0;
    for (int i = 0; i < count; i++) {
        total_qty += in_cat[i]->total_quantity;
        total_value += (double)in_cat[i]->total_quantity * in_cat[i]->unit_price;
    }

    char summary[160];
    snprintf(summary, sizeof summary, "%d produits | %d unites en stock | %.2f DT de valeur totale",
             count, total_qty, total_value);
    AppText(summary, 20, 90, 14, COLOR_TEXT_MUTED);

    const char *labels[128];
    double qty_vals[128], stock_vals[128];
    for (int i = 0; i < count; i++) {
        labels[i] = in_cat[i]->name;
        qty_vals[i] = (double)in_cat[i]->total_quantity;
        stock_vals[i] = (double)in_cat[i]->total_quantity * in_cat[i]->unit_price;
    }

    int chart_top = 120, gap = 30;
    int chart_h = GetScreenHeight() - chart_top - 30;
    int chart_w = (GetScreenWidth() - 2*20 - gap) / 2;

    Rectangle left_area  = { 20, chart_top, chart_w, chart_h };
    Rectangle right_area = { 20 + chart_w + gap, chart_top, chart_w, chart_h };

        draw_hbar_chart("Quantite par produit", left_area, labels, qty_vals, count,
                     COLOR_ACCENT_BLUE, false, "Aucun produit dans cette categorie.");
    draw_hbar_chart("Valeur du stock par produit", right_area, labels, stock_vals, count,
                     COLOR_ACCENT_TEAL, true, "Aucun produit dans cette categorie.");
}

/* Full audit trail: every stock movement across every product, most
 * recent first, paginated. Refetched fresh each frame - same "recompute
 * every draw" style already used by draw_stats_screen. */
static void draw_audit_log_screen(void) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Journal d'audit - tous les mouvements de stock", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    static Movement audit_log[500];
    static int audit_page = 0;
    int audit_count = inv_get_all_movements(audit_log, 500);

    char summary[96];
    snprintf(summary, sizeof summary, "%d mouvement(s) enregistre(s)", audit_count);
    AppText(summary, 20, 90, 14, COLOR_TEXT_MUTED);

    int table_x = 20, ty = 120;
    int col_date = table_x, col_product = table_x + 150, col_qty = table_x + 380,
        col_type = table_x + 450, col_user = table_x + 580, col_reason = table_x + 700;
    AppText("Date/heure", col_date, ty, 13, DARKGRAY);
    AppText("Produit", col_product, ty, 13, DARKGRAY);
    AppText("Qte", col_qty, ty, 13, DARKGRAY);
    AppText("Type", col_type, ty, 13, DARKGRAY);
    AppText("Utilisateur", col_user, ty, 13, DARKGRAY);
    AppText("Raison / reference", col_reason, ty, 13, DARKGRAY);

    int row_y = ty + 28;
    int AUDIT_PAGE_SIZE = 16;
    int page_count = (audit_count + AUDIT_PAGE_SIZE - 1) / AUDIT_PAGE_SIZE;
    if (audit_page >= page_count) audit_page = page_count > 0 ? page_count - 1 : 0;
    int start = audit_page * AUDIT_PAGE_SIZE;
    int shown = 0;

    if (audit_count == 0) {
        AppText("Aucun mouvement enregistre pour le moment.", table_x, row_y, 14, COLOR_TEXT_MUTED);
    }

    for (int i = start; i < audit_count && shown < AUDIT_PAGE_SIZE; i++, shown++) {
        Movement *mv = &audit_log[i];
        AppText(mv->created_at, col_date, row_y, 13, (Color){30,41,59,255});

        int prod_max_w = col_qty - col_product - 10;
        BeginScissorMode(col_product, row_y - 2, prod_max_w, 18);
        AppText(mv->product_name, col_product, row_y, 13, (Color){30,41,59,255});
        EndScissorMode();

        char delta_buf[16];
        snprintf(delta_buf, sizeof delta_buf, "%+d", mv->delta);
        AppText(delta_buf, col_qty, row_y, 13, mv->delta >= 0 ? COLOR_ACCENT_TEAL : COLOR_ACCENT_RED);

        AppText(mv->type, col_type, row_y, 13, (Color){30,41,59,255});
        AppText(mv->username[0] ? mv->username : "-", col_user, row_y, 13, COLOR_TEXT_MUTED);

        const char *detail = mv->reason[0] ? mv->reason : mv->reference;
        int reason_max_w = GetScreenWidth() - col_reason - 20;
        BeginScissorMode(col_reason, row_y - 2, reason_max_w, 18);
        AppText(detail, col_reason, row_y, 13, COLOR_TEXT_MUTED);
        EndScissorMode();

        row_y += 26;
    }

    int pag_y = GetScreenHeight() - 60;
    char page_label[32];
    snprintf(page_label, sizeof page_label, "Page %d / %d", audit_page + 1, page_count > 0 ? page_count : 1);
    DrawText(page_label, table_x, pag_y + 6, 14, DARKGRAY);
        if (GuiButton((Rectangle){ table_x + 140, pag_y, 70, 30 }, "< Prec") && audit_page > 0) audit_page--;
    if (GuiButton((Rectangle){ table_x + 220, pag_y, 70, 30 }, "Suiv >") && audit_page + 1 < page_count) audit_page++;
}

/* Archived (soft-deleted) products - viewable and restorable, so nothing
 * is ever truly gone. Fetched fresh each frame, same style as the stats
 * screens. Restoring re-adds it to the active in-memory index, so the
 * caller's total_products must be refreshed afterward. */
static void draw_archived_products_screen(Product **all_products_ptr, int *total_products_ptr, Toast *toast) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Produits archives", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    Product *archived;
    int archived_count = inv_get_archived_products(&archived);

    char summary[96];
    snprintf(summary, sizeof summary, "%d produit(s) archive(s)", archived_count);
    AppText(summary, 20, 90, 14, COLOR_TEXT_MUTED);

    int sx = 20, row_y = 130;
    if (archived_count == 0) {
        AppText("Aucun produit archive pour le moment.", sx, row_y, 14, COLOR_TEXT_MUTED);
    }

    for (int i = 0; i < archived_count; i++) {
        Product *p = &archived[i];
        Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 44 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect);
        if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
        DrawLine((int)row_rect.x, row_y + 40, (int)(row_rect.x + row_rect.width), row_y + 40, COLOR_BORDER);

        AppTextBold(p->name, sx + 8, row_y + 4, 15, (Color){30,41,59,255});
        char sub[160];
        snprintf(sub, sizeof sub, "%s | ancienne categorie: %s", p->sku, p->category[0] ? p->category : "-");
        AppText(sub, sx + 8, row_y + 24, 12, COLOR_TEXT_MUTED);

        Rectangle restore_btn = { GetScreenWidth() - sx - 130, row_y + 6, 130, 30 };
        bool restore_hover = CheckCollisionPointRec(GetMousePosition(), restore_btn);
        DrawRectangleRounded(restore_btn, 0.2f, 4, restore_hover ? (Color){13,148,105,255} : COLOR_ACCENT_TEAL);
        Vector2 rt = MeasureTextEx(g_appFont, "Restaurer", 14, 1);
        AppText("Restaurer", restore_btn.x + restore_btn.width/2 - rt.x/2, restore_btn.y + 7, 14, WHITE);
        if (restore_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            char err[256];
            if (inv_restore_product(p->id, err, sizeof err)) {
                *total_products_ptr = inv_all_products(all_products_ptr);
                toast_show(toast, "Produit restaure - assignez-lui une categorie via Modifier", false);
            } else {
                toast_show(toast, err, true);
            }
        }

               row_y += 48;
    }
}

/* One clickable alert row: name/sku on the left, category badge + qty
 * vs threshold on the right. Shared by both sections below - only the
 * accent color and which array it reads from differ. */
static int draw_alert_row(Product *p, int sx, int row_y, int width, Color accent) {
    Rectangle row_rect = { sx, row_y - 4, width, 44 };
    bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect);
    if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
    DrawLine(sx, row_y + 40, sx + width, row_y + 40, COLOR_BORDER);

    DrawRectangle(sx, row_y - 4, 4, 44, accent); /* left accent stripe */

    AppTextBold(p->name, sx + 16, row_y + 2, 15, (Color){30,41,59,255});
    AppText(p->sku, sx + 16, row_y + 22, 12, COLOR_TEXT_MUTED);

    char qty_buf[64];
    snprintf(qty_buf, sizeof qty_buf, "%d / %d (seuil)", p->total_quantity, p->alert_threshold);
    Vector2 qty_size = MeasureTextEx(g_appFontBold, qty_buf, 14, 1);
    AppTextBold(qty_buf, sx + width - qty_size.x - 160, row_y + 4, 14, accent);

    const char *cat_name = p->category[0] ? p->category : "Sans categorie";
    Vector2 cat_size = MeasureTextEx(g_appFont, cat_name, 13, 1);
    AppText(cat_name, sx + width - cat_size.x - 8, row_y + 22, 13, COLOR_ACCENT_BLUE);

    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_active_category_id = p->category_id;
        snprintf(g_active_category_name, sizeof g_active_category_name, "%s", cat_name);
        g_screen = SCREEN_MAIN;
    }
    return row_y + 48;
}

static void draw_stock_alerts_screen(Product *all_products, int total_products) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Alertes de stock", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    /* Classify every active product once per frame - cheap, same
       "recompute fresh" style already used by the stats screens. */
    Product *out_of_stock[256], *low_stock[256];
    int out_count = 0, low_count = 0;
    for (int i = 0; i < total_products; i++) {
        Product *p = &all_products[i];
        if (p->total_quantity <= 0) {
            if (out_count < 256) out_of_stock[out_count++] = p;
        } else if (p->total_quantity <= p->alert_threshold) {
            if (low_count < 256) low_stock[low_count++] = p;
        }
    }

    char summary[128];
    snprintf(summary, sizeof summary, "%d produit(s) en rupture de stock  |  %d produit(s) en stock faible",
             out_count, low_count);
    AppText(summary, 20, 90, 14, COLOR_TEXT_MUTED);

    int sx = 20, width = GetScreenWidth() - 2*sx;
    int row_y = 130;
    int MAX_SHOWN = 8;

    /* ---- Section: out of stock ---- */
    AppTextBold("Rupture de stock", sx, row_y, 16, COLOR_ACCENT_RED);
    row_y += 30;
    if (out_count == 0) {
        AppText("Aucun produit en rupture de stock.", sx, row_y, 14, COLOR_TEXT_MUTED);
        row_y += 40;
    } else {
        int shown = out_count < MAX_SHOWN ? out_count : MAX_SHOWN;
        for (int i = 0; i < shown; i++) row_y = draw_alert_row(out_of_stock[i], sx, row_y, width, COLOR_ACCENT_RED);
        if (out_count > shown) {
            char more[64];
            snprintf(more, sizeof more, "+ %d autre(s) produit(s) en rupture non affiches", out_count - shown);
            AppText(more, sx, row_y, 12, COLOR_TEXT_MUTED);
            row_y += 30;
        }
    }

    row_y += 20;

    /* ---- Section: low stock ---- */
    AppTextBold("Stock faible (sous le seuil d'alerte)", sx, row_y, 16, COLOR_ACCENT_ORANGE);
    row_y += 30;
    if (low_count == 0) {
        AppText("Aucun produit en stock faible.", sx, row_y, 14, COLOR_TEXT_MUTED);
    } else {
               int shown = low_count < MAX_SHOWN ? low_count : MAX_SHOWN;
        for (int i = 0; i < shown; i++) row_y = draw_alert_row(low_stock[i], sx, row_y, width, COLOR_ACCENT_ORANGE);
        if (low_count > shown) {
            char more[64];
            snprintf(more, sizeof more, "+ %d autre(s) produit(s) en stock faible non affiches", low_count - shown);
            AppText(more, sx, row_y, 12, COLOR_TEXT_MUTED);
        }
    }
}

/* Backup/restore screen. Backups live in backups/*.db (timestamped).
 * Restore rebuilds the whole in-memory index and forces a re-login
 * afterward, since the restored data may not even contain the current
 * session's user id anymore. */
static void draw_backup_screen(WmsDb *db, Product **all_products_ptr, int *total_products_ptr,
                                Category **all_categories_ptr, int *total_categories_ptr,
                                Toast *toast) {
    ClearBackground((Color){ 245, 247, 250, 255 });

    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Sauvegarde et restauration", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    _mkdir("backups"); /* ignored if it already exists */

    int sx = 20, sy = 100, sh = 34;
    if (GuiButton((Rectangle){ sx, sy, 260, sh }, "Creer une sauvegarde maintenant")) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char path[128];
        snprintf(path, sizeof path, "backups/backup_%04d%02d%02d_%02d%02d%02d.db",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
        char err[256];
        if (inv_backup_database(path, err, sizeof err)) {
            char msg[160];
            snprintf(msg, sizeof msg, "Sauvegarde creee: %s", path);
            toast_show(toast, msg, false);
        } else {
            toast_show(toast, err, true);
        }
    }

    AppTextBold("Sauvegardes disponibles", sx, sy + 50, 15, (Color){30,41,59,255});

    static char restore_target[300] = "";
    static bool confirm_restore = false;

    int row_y = sy + 84;
    int found = 0;
    DIR *d = opendir("backups");
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len < 4 || strcmp(entry->d_name + len - 3, ".db") != 0) continue;
            found++;

            Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 40 };
            bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect) && !confirm_restore;
            if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
            DrawLine(sx, row_y + 36, GetScreenWidth() - sx, row_y + 36, COLOR_BORDER);

            AppText(entry->d_name, sx + 8, row_y + 8, 14, (Color){30,41,59,255});

            Rectangle restore_btn = { GetScreenWidth() - sx - 130, row_y + 4, 130, 28 };
            bool rb_hover = CheckCollisionPointRec(GetMousePosition(), restore_btn) && !confirm_restore;
            DrawRectangleRounded(restore_btn, 0.2f, 4, rb_hover ? (Color){29,78,216,255} : COLOR_ACCENT_BLUE);
            Vector2 rt = MeasureTextEx(g_appFont, "Restaurer", 13, 1);
            AppText("Restaurer", restore_btn.x + restore_btn.width/2 - rt.x/2, restore_btn.y + 6, 13, WHITE);
            if (rb_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                snprintf(restore_target, sizeof restore_target, "backups/%s", entry->d_name);
                confirm_restore = true;
            }

            row_y += 44;
        }
        closedir(d);
    }
    if (found == 0) {
        AppText("Aucune sauvegarde pour le moment.", sx, row_y, 14, COLOR_TEXT_MUTED);
    }

    /* ---- Confirm restore ---- */
    if (confirm_restore) {
        Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 100, 440, 200 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Confirmer la restauration");
        float bx = box.x + 20, by = box.y + 40;
        AppText("Toutes les donnees actuelles seront remplacees par", bx, by, 14, (Color){30,41,59,255});
        AppText(TextFormat("celles de: %s", restore_target), bx, by + 20, 13, COLOR_TEXT_MUTED);
        AppText("Cette action est irreversible.", bx, by + 40, 13, COLOR_ACCENT_RED);

        by += 70;
        if (GuiButton((Rectangle){ bx, by, 170, 34 }, "Oui, restaurer")) {
            char err[256];
            if (inv_restore_database(db, restore_target, err, sizeof err)) {
                *total_products_ptr = inv_all_products(all_products_ptr);
                *total_categories_ptr = inv_get_categories(all_categories_ptr);
                toast_show(toast, "Restauration reussie - reconnexion requise", false);
                session_logout(db, &g_session);
                g_screen = SCREEN_LOGIN;
            } else {
                toast_show(toast, err, true);
            }
            confirm_restore = false;
        }
                if (GuiButton((Rectangle){ bx + 190, by, 170, 34 }, "Annuler")) confirm_restore = false;
    }
}

static void draw_suppliers_screen(WmsDb *db, Toast *toast) {
    ClearBackground((Color){ 245, 247, 250, 255 });
    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Fournisseurs", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    static Supplier suppliers[256];
    int count = db_list_suppliers(db, suppliers, 256);

    static bool show_form = false;
    static bool editing = false;
    static int edit_id = 0;
    static char f_name[128] = {0}, f_contact[128] = {0}, f_phone[32] = {0},
                f_email[128] = {0}, f_address[192] = {0};
    static bool e_name=false, e_contact=false, e_phone=false, e_email=false, e_address=false;

    bool modal_active = show_form;

    int sx = 20, sy = 100, sh = 32;
    if (GuiButton((Rectangle){ sx, sy, 200, sh }, "+ Ajouter fournisseur") && !modal_active) {
        show_form = true; editing = false; edit_id = 0;
        f_name[0]=f_contact[0]=f_phone[0]=f_email[0]=f_address[0]='\0';
        e_name = true; e_contact=e_phone=e_email=e_address=false;
    }

    int row_y = sy + sh + 30;
    if (count == 0) {
        AppText("Aucun fournisseur pour le moment.", sx, row_y, 14, COLOR_TEXT_MUTED);
    }
    for (int i = 0; i < count; i++) {
        Supplier *s = &suppliers[i];
        Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 44 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect) && !modal_active;
        if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
        DrawLine(sx, row_y+40, GetScreenWidth()-sx, row_y+40, COLOR_BORDER);

        AppTextBold(s->name, sx+8, row_y+2, 15, (Color){30,41,59,255});
        char sub[256];
        snprintf(sub, sizeof sub, "%s | %s | %s", s->contact_name[0]?s->contact_name:"-",
                 s->phone[0]?s->phone:"-", s->email[0]?s->email:"-");
        AppText(sub, sx+8, row_y+22, 12, COLOR_TEXT_MUTED);

        Rectangle mod_btn = { GetScreenWidth()-sx-190, row_y+8, 85, 28 };
        Rectangle del_btn = { GetScreenWidth()-sx-95, row_y+8, 85, 28 };
        bool mod_hover = CheckCollisionPointRec(GetMousePosition(), mod_btn) && !modal_active;
        DrawRectangleLinesEx(mod_btn, 1, COLOR_BORDER);
        if (mod_hover) DrawRectangleRec(mod_btn, (Color){239,246,255,255});
        AppText("Modifier", mod_btn.x+12, mod_btn.y+6, 13, (Color){30,41,59,255});
        if (mod_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            show_form = true; editing = true; edit_id = s->id;
            snprintf(f_name, sizeof f_name, "%s", s->name);
            snprintf(f_contact, sizeof f_contact, "%s", s->contact_name);
            snprintf(f_phone, sizeof f_phone, "%s", s->phone);
            snprintf(f_email, sizeof f_email, "%s", s->email);
            snprintf(f_address, sizeof f_address, "%s", s->address);
            e_name = true; e_contact=e_phone=e_email=e_address=false;
        }

        bool del_hover = CheckCollisionPointRec(GetMousePosition(), del_btn) && !modal_active;
        DrawRectangleLinesEx(del_btn, 1, COLOR_BORDER);
        if (del_hover) DrawRectangleRec(del_btn, (Color){254,242,242,255});
        AppText("Suppr.", del_btn.x+16, del_btn.y+6, 13, (Color){185,28,28,255});
        if (del_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            char err[256];
            if (db_delete_supplier(db, s->id, err, sizeof err)) toast_show(toast, "Fournisseur supprime", false);
            else toast_show(toast, err, true);
        }

        row_y += 48;
    }

    if (show_form) {
        Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 200, 440, 400 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, editing ? "Modifier le fournisseur" : "Nouveau fournisseur");

        bool *flags[5] = { &e_name, &e_contact, &e_phone, &e_email, &e_address };
        NavResult nav = nav_handle_focus(flags, 5);

        float bx = box.x+20, by = box.y+40;
        GuiLabel((Rectangle){bx,by,100,24}, "Nom");
        if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_name, sizeof f_name, e_name) && !nav.moved) {
            e_name=!e_name; if(e_name){e_contact=e_phone=e_email=e_address=false;}
        }
        by+=34;
        GuiLabel((Rectangle){bx,by,100,24}, "Contact");
        if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_contact, sizeof f_contact, e_contact) && !nav.moved) {
            e_contact=!e_contact; if(e_contact){e_name=e_phone=e_email=e_address=false;}
        }
        by+=34;
        GuiLabel((Rectangle){bx,by,100,24}, "Telephone");
        if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_phone, sizeof f_phone, e_phone) && !nav.moved) {
            e_phone=!e_phone; if(e_phone){e_name=e_contact=e_email=e_address=false;}
        }
        by+=34;
        GuiLabel((Rectangle){bx,by,100,24}, "Email");
        if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_email, sizeof f_email, e_email) && !nav.moved) {
            e_email=!e_email; if(e_email){e_name=e_contact=e_phone=e_address=false;}
        }
        by+=34;
        GuiLabel((Rectangle){bx,by,100,24}, "Adresse");
        if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_address, sizeof f_address, e_address) && !nav.moved) {
            e_address=!e_address; if(e_address){e_name=e_contact=e_phone=e_email=false;}
        }

        by += 50;
        if (GuiButton((Rectangle){bx,by,130,32}, "Enregistrer") || nav.submit) {
            Supplier s = {0};
            s.id = edit_id;
            snprintf(s.name, sizeof s.name, "%s", f_name);
            snprintf(s.contact_name, sizeof s.contact_name, "%s", f_contact);
            snprintf(s.phone, sizeof s.phone, "%s", f_phone);
            snprintf(s.email, sizeof s.email, "%s", f_email);
            snprintf(s.address, sizeof s.address, "%s", f_address);

            char err[256];
            bool ok = editing ? db_update_supplier(db, &s, err, sizeof err)
                              : db_create_supplier(db, &s, err, sizeof err);

            if (ok) {
                toast_show(toast, editing ? "Fournisseur modifie" : "Fournisseur ajoute", false);
                show_form = false;
            } else {
                toast_show(toast, err, true);
            }
        }
        if (GuiButton((Rectangle){bx+150,by,130,32}, "Annuler")) show_form = false;
    }
}

static void draw_purchase_orders_screen(WmsDb *db, Toast *toast) {
    ClearBackground((Color){ 245, 247, 250, 255 });
    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText("Commandes fournisseurs", 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Categories", 14, 1);
    AppText("< Categories", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_CATEGORIES;

    static PurchaseOrder pos[256];
    int count = db_list_purchase_orders(db, pos, 256);

    static bool show_form = false;
    static Supplier suppliers[256];
    static int supplier_count = 0;
    static int f_supplier_id = 0;
    static char f_supplier_name[128] = {0};
    static bool supplier_dropdown_open = false;
    static char f_reference[64] = {0};
    static bool e_reference = false;

    bool modal_active = show_form;

    int sx = 20, sy = 100, sh = 32;
    if (GuiButton((Rectangle){ sx, sy, 200, sh }, "+ Nouvelle commande") && !modal_active) {
        show_form = true;
        supplier_count = db_list_suppliers(db, suppliers, 256);
        f_supplier_id = 0; f_supplier_name[0] = '\0'; f_reference[0] = '\0';
        supplier_dropdown_open = false; e_reference = true;
    }

    int row_y = sy + sh + 30;
    AppTextBold("N. commande", sx, row_y, 13, DARKGRAY);
    AppTextBold("Fournisseur", sx+120, row_y, 13, DARKGRAY);
    AppTextBold("Statut", sx+380, row_y, 13, DARKGRAY);
    AppTextBold("Cree le", sx+520, row_y, 13, DARKGRAY);
    row_y += 28;

    if (count == 0) {
        AppText("Aucune commande pour le moment.", sx, row_y, 14, COLOR_TEXT_MUTED);
    }
    for (int i = 0; i < count; i++) {
        PurchaseOrder *po = &pos[i];
        Rectangle row_rect = { sx, row_y - 4, GetScreenWidth() - 2*sx, 32 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row_rect) && !modal_active;
        if (hover) DrawRectangleRec(row_rect, (Color){235,240,248,255});
        DrawLine(sx, row_y+28, GetScreenWidth()-sx, row_y+28, COLOR_BORDER);

        AppText(po->po_number, sx, row_y+4, 14, (Color){30,41,59,255});
        AppText(po->supplier_name, sx+120, row_y+4, 14, (Color){30,41,59,255});

        Color status_color = COLOR_TEXT_MUTED;
        if (strcmp(po->status, "recu") == 0) status_color = COLOR_ACCENT_TEAL;
        else if (strcmp(po->status, "recu_partiel") == 0) status_color = COLOR_ACCENT_ORANGE;
        else if (strcmp(po->status, "annule") == 0) status_color = COLOR_ACCENT_RED;
        AppText(po->status, sx+380, row_y+4, 14, status_color);
        AppText(po->created_at, sx+520, row_y+4, 13, COLOR_TEXT_MUTED);

        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_active_po_id = po->id;
            snprintf(g_active_po_number, sizeof g_active_po_number, "%s", po->po_number);
            g_screen = SCREEN_PO_DETAIL;
        }
        row_y += 36;
    }

    if (show_form) {
        Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 130, 440, 260 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Nouvelle commande");

        float bx = box.x+20, by = box.y+40;
        GuiLabel((Rectangle){bx,by,100,24}, "Fournisseur");
        Rectangle sup_field = { bx+110, by, 280, 24 };
        bool sup_hover = CheckCollisionPointRec(GetMousePosition(), sup_field);
        DrawRectangleRec(sup_field, WHITE);
        DrawRectangleLinesEx(sup_field, 1, supplier_dropdown_open ? COLOR_ACCENT_BLUE : COLOR_BORDER);
        AppText(f_supplier_name[0] ? f_supplier_name : "Choisir un fournisseur (optionnel)",
                sup_field.x+8, sup_field.y+5, 13, f_supplier_name[0] ? (Color){30,41,59,255} : COLOR_TEXT_MUTED);
        if (sup_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            supplier_dropdown_open = !supplier_dropdown_open;
            e_reference = false;
        }

        by += 34;
        GuiLabel((Rectangle){bx,by,100,24}, "Reference");
        if (!supplier_dropdown_open) {
            bool *flags[1] = { &e_reference };
            NavResult nav = nav_handle_focus(flags, 1);
            if (GuiTextBox((Rectangle){bx+110,by,280,24}, f_reference, sizeof f_reference, e_reference) && !nav.moved)
                e_reference = !e_reference;
        }

        by += 50;
        if (!supplier_dropdown_open) {
            if (GuiButton((Rectangle){bx,by,130,32}, "Creer")) {
                int new_po_id;
                char err[256];
                if (db_create_purchase_order(db, f_supplier_id, g_session.user_id, f_reference,
                                              &new_po_id, err, sizeof err)) {
                    g_active_po_id = new_po_id;
                    PurchaseOrder tmp[256];
                    int c = db_list_purchase_orders(db, tmp, 256);
                    for (int i = 0; i < c; i++)
                        if (tmp[i].id == new_po_id)
                            snprintf(g_active_po_number, sizeof g_active_po_number, "%s", tmp[i].po_number);
                    toast_show(toast, "Commande creee", false);
                    show_form = false;
                    g_screen = SCREEN_PO_DETAIL;
                } else {
                    toast_show(toast, err, true);
                }
            }
            if (GuiButton((Rectangle){bx+150,by,130,32}, "Annuler")) show_form = false;
        }

        if (supplier_dropdown_open) {
            float row_h = 26;
            Rectangle dd = { sup_field.x, sup_field.y + sup_field.height + 2, sup_field.width, row_h * supplier_count };
            DrawRectangleRec(dd, WHITE);
            DrawRectangleLinesEx(dd, 1, COLOR_BORDER);
            for (int i = 0; i < supplier_count; i++) {
                Rectangle row = { dd.x, dd.y + i*row_h, dd.width, row_h };
                bool hover = CheckCollisionPointRec(GetMousePosition(), row);
                if (hover) DrawRectangleRec(row, (Color){239,246,255,255});
                AppText(suppliers[i].name, row.x+8, row.y+5, 13, (Color){30,41,59,255});
                if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    f_supplier_id = suppliers[i].id;
                    snprintf(f_supplier_name, sizeof f_supplier_name, "%s", suppliers[i].name);
                    supplier_dropdown_open = false;
                }
            }
        }
    }
}

static void draw_po_detail_screen(WmsDb *db, Toast *toast) {
    ClearBackground((Color){ 245, 247, 250, 255 });
    DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
    if (g_logoLoaded)
        DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0, 34.0f / g_logoLockupDark.height, WHITE);
    AppText(TextFormat("Commande %s", g_active_po_number), 20, 46, 14, (Color){180,190,210,255});

    Rectangle back_btn = { GetScreenWidth() - 150, 20, 130, 32 };
    bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn);
    DrawRectangleRounded(back_btn, 0.2f, 6, back_hover ? (Color){37,54,88,255} : (Color){71,85,105,255});
    Vector2 back_ts = MeasureTextEx(g_appFont, "< Commandes", 14, 1);
    AppText("< Commandes", back_btn.x + back_btn.width/2 - back_ts.x/2, back_btn.y + 9, 14, WHITE);
    if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_screen = SCREEN_PURCHASE_ORDERS;

    static PurchaseOrderItem items[256];
    int count = db_get_po_items(db, g_active_po_id, items, 256);

    static bool show_add_item = false;
    static char f_sku[64] = {0};
    static char f_qty[16] = {0};
    static char f_cost[32] = {0};
    static bool e_sku=false, e_qty=false, e_cost=false;

    static bool show_receive = false;
    static int receive_item_id = 0, receive_product_id = 0, receive_already = 0, receive_ordered = 0;
    static char receive_po_number[32] = {0};
    static char r_qty[16] = {0};
    static bool e_rqty = false;

    bool modal_active = show_add_item || show_receive;

    int sx = 20, sy = 100, sh = 32;
    if (GuiButton((Rectangle){ sx, sy, 170, sh }, "+ Ajouter article") && !modal_active) {
        show_add_item = true;
        f_sku[0]=f_qty[0]=f_cost[0]='\0';
        e_sku = true; e_qty=e_cost=false;
    }

    int row_y = sy + sh + 30;
    AppTextBold("SKU", sx, row_y, 13, DARKGRAY);
    AppTextBold("Produit", sx+110, row_y, 13, DARKGRAY);
    AppTextBold("Commande", sx+380, row_y, 13, DARKGRAY);
    AppTextBold("Recu", sx+470, row_y, 13, DARKGRAY);
    AppTextBold("Cout unit.", sx+540, row_y, 13, DARKGRAY);
    row_y += 28;

    if (count == 0) {
        AppText("Aucun article - cliquez sur \"+ Ajouter article\".", sx, row_y, 14, COLOR_TEXT_MUTED);
    }
    for (int i = 0; i < count; i++) {
        PurchaseOrderItem *it = &items[i];
        bool fully_received = it->quantity_received >= it->quantity_ordered;

        AppText(it->product_sku, sx, row_y, 13, (Color){30,41,59,255});
        AppText(it->product_name, sx+110, row_y, 13, (Color){30,41,59,255});
        DrawText(TextFormat("%d", it->quantity_ordered), sx+380, row_y, 13, BLACK);
        DrawText(TextFormat("%d", it->quantity_received), sx+470, row_y,
                 13, fully_received ? (Color){16,150,90,255} : (Color){200,120,20,255});
        DrawText(TextFormat("%.2f", it->unit_cost), sx+540, row_y, 13, BLACK);

        Rectangle recv_btn = { GetScreenWidth() - sx - 110, row_y - 4, 110, 26 };
        bool recv_hover = CheckCollisionPointRec(GetMousePosition(), recv_btn) && !modal_active && !fully_received;
        DrawRectangleRounded(recv_btn, 0.2f, 4, fully_received ? (Color){200,200,200,255} :
                              (recv_hover ? (Color){13,148,105,255} : COLOR_ACCENT_TEAL));
        AppText(fully_received ? "Complete" : "Recevoir", recv_btn.x+14, recv_btn.y+5, 13, WHITE);
        if (recv_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            show_receive = true;
            receive_item_id = it->id;
            receive_product_id = it->product_id;
            receive_already = it->quantity_received;
            receive_ordered = it->quantity_ordered;
            snprintf(receive_po_number, sizeof receive_po_number, "%s", g_active_po_number);
            r_qty[0] = '\0';
            e_rqty = true;
        }

        row_y += 30;
    }

    if (show_add_item) {
        Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 130, 400, 260 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Ajouter un article");

        bool *flags[3] = { &e_sku, &e_qty, &e_cost };
        NavResult nav = nav_handle_focus(flags, 3);

        float bx = box.x+20, by = box.y+40;
        GuiLabel((Rectangle){bx,by,100,24}, "SKU produit");
        if (GuiTextBox((Rectangle){bx+110,by,220,24}, f_sku, sizeof f_sku, e_sku) && !nav.moved) {
            e_sku=!e_sku; if(e_sku){e_qty=e_cost=false;}
        }
        by += 34;
        GuiLabel((Rectangle){bx,by,100,24}, "Quantite");
        if (GuiTextBox((Rectangle){bx+110,by,120,24}, f_qty, sizeof f_qty, e_qty) && !nav.moved) {
            e_qty=!e_qty; if(e_qty){e_sku=e_cost=false;}
        }
        by += 34;
        GuiLabel((Rectangle){bx,by,100,24}, "Cout unitaire");
        if (GuiTextBox((Rectangle){bx+110,by,120,24}, f_cost, sizeof f_cost, e_cost) && !nav.moved) {
            e_cost=!e_cost; if(e_cost){e_sku=e_qty=false;}
        }

        by += 50;
        if (GuiButton((Rectangle){bx,by,120,32}, "Ajouter") || nav.submit) {
            Product *p = inv_find_by_sku(f_sku);
            if (!p) {
                toast_show(toast, "SKU introuvable", true);
            } else if (!str_is_integer(f_qty) || atoi(f_qty) <= 0) {
                toast_show(toast, "Quantite invalide", true);
            } else if (!str_is_decimal(f_cost) || atof(f_cost) < 0) {
                toast_show(toast, "Cout unitaire invalide", true);
            } else {
                char err[256];
                if (db_add_po_item(db, g_active_po_id, p->id, atoi(f_qty), atof(f_cost), err, sizeof err)) {
                    db_update_po_status(db, g_active_po_id, "commande");
                    toast_show(toast, "Article ajoute", false);
                    show_add_item = false;
                } else {
                    toast_show(toast, err, true);
                }
            }
        }
        if (GuiButton((Rectangle){bx+140,by,120,32}, "Annuler")) show_add_item = false;
    }

    if (show_receive) {
        Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 100, 400, 200 };
        DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
        GuiPanel(box, "Reception de l'article");

        float bx = box.x+20, by = box.y+40;
        AppText(TextFormat("Deja recu: %d / %d commande(s)", receive_already, receive_ordered),
                bx, by, 14, COLOR_TEXT_MUTED);

        by += 30;
        if (!e_rqty) e_rqty = true;
        bool *flags[1] = { &e_rqty };
        NavResult nav = nav_handle_focus(flags, 1);
        GuiLabel((Rectangle){bx,by,220,24}, "Nouvelle quantite recue (total)");
        if (GuiTextBox((Rectangle){bx,by+26,120,24}, r_qty, sizeof r_qty, e_rqty) && !nav.moved)
            e_rqty = !e_rqty;

        by += 66;
        if (GuiButton((Rectangle){bx,by,130,32}, "Confirmer") || nav.submit) {
            if (!str_is_integer(r_qty)) {
                toast_show(toast, "Quantite invalide", true);
            } else {
                int new_total = atoi(r_qty);
                if (new_total <= receive_already || new_total > receive_ordered) {
                    toast_show(toast, "Doit depasser le deja-recu, sans depasser la commande", true);
                } else {
                    char err[256];
                    if (inv_receive_po_item(g_active_po_id, receive_item_id, receive_product_id,
                                             receive_already, new_total, receive_po_number,
                                             &g_session, err, sizeof err)) {
                        toast_show(toast, "Reception enregistree", false);
                        show_receive = false;
                    } else {
                        toast_show(toast, err, true);
                    }
                }
            }
        }
        if (GuiButton((Rectangle){bx+150,by,130,32}, "Annuler")) show_receive = false;
    }
}


static void AppText(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(g_appFont, text, (Vector2){ (float)x, (float)y }, (float)size, 1.0f, color);
}

static void AppTextBold(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(g_appFontBold, text, (Vector2){ (float)x, (float)y }, (float)size, 1.0f, color);
}

void gui_run(WmsDb *db) {
        InitWindow(SCREEN_W, SCREEN_H, "Gestion de Stock - Warehouse WMS");
    SetWindowState(FLAG_WINDOW_RESIZABLE);

    Image iconImg = LoadImage("resources/icons/logo_mark_256.png");
    if (iconImg.data != NULL) {
        SetWindowIcon(iconImg);
        UnloadImage(iconImg);
    }
    SetTargetFPS(60);

    /* Load Segoe UI per spec; fall back to raylib's default if unavailable
       (e.g. running on a non-Windows box during testing). */
        /* Load Inter with French accented characters included (é, è, à, ç, ù, etc.)
       - without an explicit codepoint list, raylib only loads ASCII and
       accented letters render as boxes/blanks. */
    int codepoints[512];
    int cp_count = 0;
    for (int c = 32; c < 128; c++) codepoints[cp_count++] = c;       /* ASCII */
    int french_extra[] = { 0xE9,0xE8,0xEA,0xEB,0xE0,0xE2,0xE7,0xF9,
                            0xFB,0xFC,0xEE,0xEF,0xF4,0xC9,0xC8,0xC0,0xC7 };
    for (int i = 0; i < (int)(sizeof french_extra / sizeof french_extra[0]); i++)
        codepoints[cp_count++] = french_extra[i];

    Font appFont = LoadFontEx("resources/fonts/Inter-Regular.ttf", 48, codepoints, cp_count);
    g_logoLockupLight = LoadTexture("resources/icons/logo_lockup_light.png");
    g_logoLockupDark  = LoadTexture("resources/icons/logo_lockup_dark.png");
    g_logoLoaded = (g_logoLockupLight.id != 0 && g_logoLockupDark.id != 0);
    g_logoMark = LoadTexture("resources/icons/logo_mark_256.png");
    SetTextureFilter(appFont.texture, TEXTURE_FILTER_BILINEAR);
    GuiSetFont(appFont);
    g_appFont = appFont;   /* see Part 2 - stored globally so DrawText calls can use it too */

    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 1);

    /* Global control palette - applies to every button/textbox/panel
       unless overridden per-control. */
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL,   ColorToInt(WHITE));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,  ColorToInt((Color){ 239, 246, 255, 255 }));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED,  ColorToInt((Color){ 219, 234, 254, 255 }));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(COLOR_BORDER));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED,ColorToInt(COLOR_ACCENT_BLUE));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,   ColorToInt((Color){ 30, 41, 59, 255 }));
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);

    /* ---- state ---- */
    char search_query[128] = {0};
    bool search_edit = false;

    Product *all_products;
    int total_products = inv_all_products(&all_products);
    int page = 0;

    Product *filtered[WMS_MAX_PRODUCTS];
    int filtered_count = 0;
    bool is_filtering = false;

    int selected_product_id = -1; /* database id of the selected row - stable
                                      across filtering/category views, unlike
                                      an array index which meant different
                                      things depending on which list was active */

    ActivePanel panel = PANEL_NONE;
    Toast toast = {0};

    /* Add-product form fields */
    char f_sku[64] = {0}, f_name[128] = {0}, f_category[64] = {0};
    char f_price[32] = {0}, f_threshold[16] = {0}, f_initial_qty[16] = {0};
    char f_unit[16] = "piece";
    bool edit_sku = false, edit_name = false, edit_cat = false,
         edit_price = false, edit_threshold = false, edit_initial_qty = false;

        /* Category select-dropdown state */
    int  f_category_id = 0;         /* 0 = none chosen yet */
    bool cat_dropdown_open = false;
    bool cat_adding_new = false;
    char f_new_cat_input[64] = {0};
    bool edit_new_cat = false;
    Rectangle cat_field_rect = {0};  /* filled in when the field is drawn, read by the overlay at the end */

    /* Unit select-dropdown state (Add product form) */
    bool unit_dropdown_open = false;
    Rectangle unit_field_rect = {0};

    Category *all_categories;
    int total_categories = inv_get_categories(&all_categories);

    /* Movement form fields */
    char m_qty[16] = {0};
    char m_reason[128] = {0};
    bool edit_qty = false;
    bool edit_reason = false;
    int  movement_sign = 1; /* +1 in, -1 out */

    /* Movement history panel state */
    Movement mv_history[64];
    int mv_history_count = 0;

       /* Edit-product form - reuses the same field pattern as add-product,
       plus the id/version of the product being edited (needed for
       inv_update_product's optimistic-lock check). */
    int  edit_product_id = 0, edit_product_version = 0;
    char e_name[128] = {0}, e_price[32] = {0}, e_threshold[16] = {0};
    bool ee_name = false, ee_price = false, ee_threshold = false;
    int  e_category_id = 0;
    char e_category[64] = {0};
    bool e_cat_dropdown_open = false;
    Rectangle e_cat_field_rect = {0};
    char e_unit[16] = "piece";
    bool e_unit_dropdown_open = false;
    Rectangle e_unit_field_rect = {0};

    /* Category management panel */
    int delete_category_id = 0;
    char delete_category_name[64] = {0};
    int  rename_category_id = 0;
    char rename_category_input[64] = {0};
    bool rename_category_edit = false;
    /* User management panel (admin only) */
    int  mgmt_user_ids[64];
    char mgmt_user_names[64][64];
    char mgmt_user_roles[64][16];
    int  mgmt_user_count = 0;

        /* Categories home screen state */
    char cat_search[128] = {0};
    bool cat_search_edit = false;
    char global_product_search[128] = {0};
    bool global_search_edit = false;
    int  cat_action_id = 0;         /* which category a rename/delete targets */
    char cat_rename_input[64] = {0};
    bool cat_rename_edit = false;
    bool cat_renaming = false;      /* inline rename mode active for cat_action_id */
    ActivePanel cat_screen_panel = PANEL_NONE; /* reuses your existing panel enum for Add/Delete-confirm dialogs on this screen */

    while (!WindowShouldClose()) {

        /* ---- session bookkeeping (runs every frame, before anything else) ---- */
        if (g_session.active &&
            (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
            session_touch(&g_session);
        }
                if ((g_screen == SCREEN_MAIN || g_screen == SCREEN_CATEGORIES) &&
            session_is_locked(&g_session, 300)) {
            g_screen = SCREEN_LOGIN;
            login_password[0] = '\0';
        }

                /* ---- splash screen: shows once at startup, then auto-advances ---- */
        if (g_screen == SCREEN_SPLASH) {
            BeginDrawing();
            draw_splash_screen();
            EndDrawing();
            continue;
        }

        /* ---- login screen: draw it and skip everything else this frame ---- */
        if (g_screen == SCREEN_LOGIN) {
            BeginDrawing();
            draw_login_screen(db);
            EndDrawing();
            continue;
        }

        /* ---- categories home screen ---- */
        if (g_screen == SCREEN_CATEGORIES) {
            if (cat_screen_panel == PANEL_MANAGE_USERS)
                mgmt_user_count = db_list_users(db, mgmt_user_ids, mgmt_user_names, mgmt_user_roles, 64);
            BeginDrawing();
        draw_categories_screen(db, all_categories, &total_categories,
                                    cat_search, &cat_search_edit,
                                    &cat_action_id, cat_rename_input, &cat_rename_edit,
                                    &cat_renaming, &cat_screen_panel, &toast,
                                    mgmt_user_ids, mgmt_user_names, mgmt_user_roles, &mgmt_user_count,
                                    global_product_search, &global_search_edit);
            EndDrawing();
            continue;
        }

                /* ---- statistics screen (all categories) ---- */
        if (g_screen == SCREEN_STATS) {
            BeginDrawing();
            draw_stats_screen(all_categories, total_categories, all_products, total_products);
            EndDrawing();
            continue;
        }

        /* ---- statistics screen (single category, products) ---- */
        if (g_screen == SCREEN_CATEGORY_STATS) {
            BeginDrawing();
            draw_category_stats_screen(all_products, total_products);
            EndDrawing();
            continue;
        }

                /* ---- audit log screen (every movement, every product) ---- */
        if (g_screen == SCREEN_AUDIT_LOG) {
            BeginDrawing();
            draw_audit_log_screen();
            EndDrawing();
            continue;
        }

        /* ---- archived products screen ---- */
        if (g_screen == SCREEN_ARCHIVED_PRODUCTS) {
            BeginDrawing();
            draw_archived_products_screen(&all_products, &total_products, &toast);
            EndDrawing();
            continue;
        }

        /* ---- stock alerts dashboard (low stock + out of stock) ---- */
        if (g_screen == SCREEN_STOCK_ALERTS) {
            BeginDrawing();
            draw_stock_alerts_screen(all_products, total_products);
            EndDrawing();
            continue;
        }

                /* ---- backup / restore screen ---- */
        if (g_screen == SCREEN_BACKUP) {
            BeginDrawing();
            draw_backup_screen(db, &all_products, &total_products, &all_categories, &total_categories, &toast);
            EndDrawing();
            continue;
        }

        /* ---- suppliers screen ---- */
        if (g_screen == SCREEN_SUPPLIERS) {
            BeginDrawing();
            draw_suppliers_screen(db, &toast);
            EndDrawing();
            continue;
        }

        /* ---- purchase orders list ---- */
        if (g_screen == SCREEN_PURCHASE_ORDERS) {
            BeginDrawing();
            draw_purchase_orders_screen(db, &toast);
            EndDrawing();
            continue;
        }

        /* ---- purchase order detail / receiving ---- */
        if (g_screen == SCREEN_PO_DETAIL) {
            BeginDrawing();
            draw_po_detail_screen(db, &toast);
            EndDrawing();
            continue;
        }

        /* ---- update (main screen only, reached once logged in) ---- */
        if (toast.timer > 0) toast.timer -= GetFrameTime();

               if (strlen(search_query) > 0) {
            /* Search globally first, then narrow to the active category so
               results never leak in from other categories while you're
               inside one. */
            Product *raw_matches[WMS_MAX_PRODUCTS];
            int raw_count = inv_search(search_query, raw_matches, WMS_MAX_PRODUCTS);
            filtered_count = 0;
            for (int mi = 0; mi < raw_count; mi++) {
                if (g_active_category_id <= 0 || raw_matches[mi]->category_id == g_active_category_id) {
                    filtered[filtered_count++] = raw_matches[mi];
                }
            }
            is_filtering = true;
        } else {
            is_filtering = false;
            filtered_count = 0;
        }

        Product **visible_list = is_filtering ? filtered : NULL;
        (void)visible_list;
        /* Build the category-filtered view fresh each frame. Search still
          narrows within it via inv_search on top. */
        static Product *cat_filtered[WMS_MAX_PRODUCTS];
        int cat_filtered_count = 0;
        if (g_active_category_id > 0) {
            for (int pi = 0; pi < total_products; pi++)
                if (all_products[pi].category_id == g_active_category_id)
                    cat_filtered[cat_filtered_count++] = &all_products[pi];
        }

        int visible_count = is_filtering ? filtered_count
                            : (g_active_category_id > 0 ? cat_filtered_count : total_products);

        int page_count = (visible_count + PAGE_SIZE - 1) / PAGE_SIZE;
        if (page >= page_count) page = page_count > 0 ? page_count - 1 : 0;
        bool modal_active = (panel != PANEL_NONE);
        /* ---- draw ---- */
        BeginDrawing();
        ClearBackground((Color){ 245, 247, 250, 255 }); /* light neutral bg */

        /* Header bar - navy */
                DrawRectangle(0, 0, GetScreenWidth(), 74, (Color){ 21, 41, 71, 255 });
                float header_logo_h = 34.0f;
        if (g_logoLoaded) {
            DrawTextureEx(g_logoLockupDark, (Vector2){ 20, 8 }, 0,
                          header_logo_h / g_logoLockupDark.height, WHITE);
        } else {
            AppTextBold("WAREHOUSE WMS", 20, 20, 22, RAYWHITE);
        }

        Rectangle back_btn = { 200, 12, 130, 30 };
        bool back_hover = CheckCollisionPointRec(GetMousePosition(), back_btn) && !modal_active;
        if (back_hover) DrawRectangleRounded(back_btn, 0.2f, 4, (Color){37,54,88,255});
        AppText("< Categories", back_btn.x + 10, back_btn.y + 7, 13, RAYWHITE);
        if (back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_screen = SCREEN_CATEGORIES;
            EndDrawing();
            continue;
        }
        AppText(g_active_category_name, 340, 20, 16, (Color){180,190,210,255});

        /* Right-aligned cluster: logout button anchored to the edge,
           username text placed to its LEFT with a real gap - measured,
           not guessed, so it can never collide with the button. */
        int btn_w = 110, btn_h = 32, margin = 20;
        Rectangle logout_rect = { GetScreenWidth() - margin - btn_w, 16, btn_w, btn_h };

        char user_label[96];
        snprintf(user_label, sizeof user_label, "%s (%s)", g_session.username, g_session.role);
        Vector2 user_size = MeasureTextEx(g_appFont, user_label, 14, 1);
        AppText(user_label, logout_rect.x - 16 - user_size.x, 24, 14, (Color){180,190,210,255});

        bool logout_hover = CheckCollisionPointRec(GetMousePosition(), logout_rect);
        DrawRectangleRounded(logout_rect, 0.25f, 6, logout_hover ? (Color){153,27,27,255} : (Color){71,85,105,255});
        Vector2 logout_tsize = MeasureTextEx(g_appFont, "Deconnexion", 14, 1);
        AppText("Deconnexion", logout_rect.x + logout_rect.width/2 - logout_tsize.x/2,
                 logout_rect.y + 8, 14, WHITE);

                int cat_count_now = 0;
        if (g_active_category_id > 0) {
            for (int pi = 0; pi < total_products; pi++)
                if (all_products[pi].category_id == g_active_category_id) cat_count_now++;
        }
        char subtitle[64];
        int shown_count = (g_active_category_id > 0) ? cat_count_now : total_products;
        snprintf(subtitle, sizeof subtitle, "%d produits", shown_count);
        AppText(subtitle, 20, 8 + header_logo_h + 4, 14, (Color){180,190,210,255});





        if (logout_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            session_logout(db, &g_session);
            g_screen = SCREEN_LOGIN;
            login_username[0] = login_password[0] = '\0';
            EndDrawing();
            continue;
        }

        /* Search bar - own row */

        int sx = 20, sy = 94, sw = 400, sh = 32;
        if (GuiTextBox((Rectangle){ sx, sy, sw, sh }, search_query, sizeof search_query, search_edit))
            search_edit = !search_edit;
        DrawText("F3: recherche", sx, sy + sh + 4, 12, GRAY);

        /* Action buttons - own row below the search bar, so they never run
           off the right edge at narrower window sizes. Wraps naturally
           since each button's x is computed from the previous one's width
           rather than a hardcoded offset from the search bar. */

        int toolbar_y = sy + sh + 22;
        int tx = sx;
        int btn_gap = 10;

            toolbar_wrap(&tx, &toolbar_y, 160, sx, sh);
            if (GuiButton((Rectangle){ tx, toolbar_y, 160, sh }, "+ Nouveau produit")) {
            panel = PANEL_ADD_PRODUCT;
            f_sku[0] = f_name[0] = f_price[0] = f_threshold[0] = f_initial_qty[0] = '\0';
            snprintf(f_unit, sizeof f_unit, "piece");
            cat_dropdown_open = false; cat_adding_new = false; unit_dropdown_open = false; f_new_cat_input[0] = '\0';

            /* Pre-fill with whichever category we're currently browsing,
               since a product added from inside "materiel bureautique"
               should land in that category by default. */
            if (g_active_category_id > 0) {
                f_category_id = g_active_category_id;
                snprintf(f_category, sizeof f_category, "%s", g_active_category_name);
            } else {
                f_category_id = 0;
                f_category[0] = '\0';
            }
        }
        tx += 160 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 140, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 140, sh }, "Mouvement stock")) {
            if (selected_product_id > 0) { panel = PANEL_MOVEMENT; m_qty[0] = '\0'; m_reason[0] = '\0'; }
            else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
                tx += 140 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 120, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 120, sh }, "Historique")) {
            if (selected_product_id > 0) {
                mv_history_count = inv_get_movements(selected_product_id, mv_history, 64);
                panel = PANEL_MOVEMENT_HISTORY;
            } else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
               tx += 120 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 100, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 100, sh }, "Modifier")&& !modal_active) {
            Product *p = find_product_by_id(all_products, total_products, selected_product_id);
            if (p) {
                edit_product_id = p->id;
                edit_product_version = p->version;
                snprintf(e_name, sizeof e_name, "%s", p->name);
                snprintf(e_price, sizeof e_price, "%.2f", p->unit_price);
                snprintf(e_threshold, sizeof e_threshold, "%d", p->alert_threshold);
                e_category_id = p->category_id;
                snprintf(e_category, sizeof e_category, "%s", p->category);
                snprintf(e_unit, sizeof e_unit, "%s", p->unit[0] ? p->unit : "piece");
                panel = PANEL_EDIT_PRODUCT;
                ee_name = true; ee_price = ee_threshold = false;
            } else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
        tx += 100 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 100, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 100, sh }, "Archiver")) {
            Product *p = find_product_by_id(all_products, total_products, selected_product_id);
            if (p) {
                edit_product_id = p->id;
                panel = PANEL_CONFIRM_DELETE_PRODUCT;
            } else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
                tx += 110 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 150, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 150, sh }, "Gerer categories")&& !modal_active) {
            panel = PANEL_MANAGE_CATEGORIES;
        }
        tx += 150 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 130, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 130, sh }, "Statistiques")&& !modal_active) {
            g_screen = SCREEN_CATEGORY_STATS;
        }
        tx += 130 + btn_gap;

               toolbar_wrap(&tx, &toolbar_y, 140, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 140, sh }, "Exporter Excel") && !modal_active) {
            run_excel_export(g_active_category_id, g_active_category_name, &toast);
        }
        tx += 140 + btn_gap;

        toolbar_wrap(&tx, &toolbar_y, 110, sx, sh);
        if (GuiButton((Rectangle){ tx, toolbar_y, 110, sh }, "Fiche PDF") && !modal_active) {
            Product *p = find_product_by_id(all_products, total_products, selected_product_id);
            run_product_sheet_export(p, &toast);
        }
        tx += 110 + btn_gap;

        if (session_can(&g_session, "user.manage")) {
            toolbar_wrap(&tx, &toolbar_y, 160, sx, sh);
            if (GuiButton((Rectangle){ tx, toolbar_y, 160, sh }, "Gerer utilisateurs")&& !modal_active) {
                mgmt_user_count = db_list_users(db, mgmt_user_ids, mgmt_user_names, mgmt_user_roles, 64);
                panel = PANEL_MANAGE_USERS;
            }
        }

        /* Table header */
        int table_x = 20, ty = toolbar_y + sh + 24;
        int col_sku = table_x, col_name = table_x + 110, col_cat = table_x + 380,
           col_qty = table_x + 540, col_price = table_x + 640, col_alert = table_x + 760;
        AppText("SKU", col_sku, ty, 14, DARKGRAY);
        AppText("Nom", col_name, ty, 14, DARKGRAY);
        AppText("Categorie", col_cat, ty, 14, DARKGRAY);
        AppText("Qte", col_qty, ty, 14, DARKGRAY);
        AppText("Prix", col_price, ty, 14, DARKGRAY);
        AppText("Statut", col_alert, ty, 14, DARKGRAY);
        int row_y = ty + 32;
        int start = page * PAGE_SIZE;
        int shown_this_page = 0;

        for (int i = start; i < visible_count && shown_this_page < PAGE_SIZE; i++, shown_this_page++) {
                       Product *p = is_filtering ? filtered[i]
                        : (g_active_category_id > 0 ? cat_filtered[i] : &all_products[i]);
            Rectangle row_rect = { table_x - 4, row_y - 4, GetScreenWidth() - 2 * table_x + 8, 26 };
            bool hovered = CheckCollisionPointRec(GetMousePosition(), row_rect);
            bool selected = (selected_product_id == p->id);

            if (selected) DrawRectangleRec(row_rect, (Color){ 210, 225, 245, 255 });
            else if (hovered) DrawRectangleRec(row_rect, (Color){ 235, 240, 248, 255 });

            if (hovered && !modal_active && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) selected_product_id = p->id;

            AppText(p->sku, col_sku, row_y, 14, BLACK);
            AppText(p->name, col_name, row_y, 14, BLACK);
           int cat_max_width = col_qty - col_cat - 10; /* leave a 10px gap before Qte column */
           BeginScissorMode(col_cat, row_y - 2, cat_max_width, 20);
           AppText(p->category, col_cat, row_y, 14, BLACK);
           EndScissorMode();

            char qty_buf[16];
            snprintf(qty_buf, sizeof qty_buf, "%d", p->total_quantity);
            DrawText(qty_buf, col_qty, row_y, 14, BLACK);

            char price_buf[24];
            snprintf(price_buf, sizeof price_buf, "%.2f", p->unit_price);
            DrawText(price_buf, col_price, row_y, 14, BLACK);

                        if (p->total_quantity <= p->alert_threshold) {
                DrawText("STOCK FAIBLE", col_alert, row_y, 12, (Color){ 200, 40, 40, 255 });
            } else {
                DrawText("OK", col_alert, row_y, 12, (Color){ 40, 150, 70, 255 });
            }

            Rectangle row_modify_btn = { GetScreenWidth() - table_x - 90, row_y - 4, 80, 22 };
            bool row_modify_hover = CheckCollisionPointRec(GetMousePosition(), row_modify_btn) && !modal_active;
            DrawRectangleLinesEx(row_modify_btn, 1, COLOR_BORDER);
            if (row_modify_hover) DrawRectangleRec(row_modify_btn, (Color){239,246,255,255});
            AppText("Modifier", row_modify_btn.x + 8, row_modify_btn.y + 4, 12, (Color){30,41,59,255});
            if (row_modify_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                selected_product_id = p->id;
                edit_product_id = p->id;
                edit_product_version = p->version;
                snprintf(e_name, sizeof e_name, "%s", p->name);
                snprintf(e_price, sizeof e_price, "%.2f", p->unit_price);
                snprintf(e_threshold, sizeof e_threshold, "%d", p->alert_threshold);
                e_category_id = p->category_id;
                snprintf(e_category, sizeof e_category, "%s", p->category);
                snprintf(e_unit, sizeof e_unit, "%s", p->unit[0] ? p->unit : "piece");
                panel = PANEL_EDIT_PRODUCT;
                ee_name = true; ee_price = ee_threshold = false;
            }

            row_y += 30;
        }

        /* Pagination controls */
        int pag_y = GetScreenHeight() - 60;
        char page_label[32];
        snprintf(page_label, sizeof page_label, "Page %d / %d", page + 1, page_count > 0 ? page_count : 1);
        DrawText(page_label, table_x, pag_y + 6, 14, DARKGRAY);
        if (GuiButton((Rectangle){ table_x + 140, pag_y, 70, 30 }, "< Prec") && !modal_active && page > 0) page--;
        if (GuiButton((Rectangle){ table_x + 220, pag_y, 70, 30 }, "Suiv >") && !modal_active && page + 1 < page_count) page++;
        /* ---- Add product panel (modal-ish) ---- */
            if (panel == PANEL_ADD_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 250, 440, 520 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Nouveau produit");

                       bool *edit_flags[5] = { &edit_sku, &edit_name,
                                     &edit_price, &edit_threshold, &edit_initial_qty };
            bool any_focused = edit_sku || edit_name || edit_price ||
                                edit_threshold || edit_initial_qty ||
                                cat_dropdown_open || edit_new_cat || unit_dropdown_open;
            if (!any_focused) edit_sku = true;

            /* While the category or unit dropdown is active, the 5-field
               Tab/↑↓ navigation must not run at all - otherwise it fights
               with the dropdown for focus every frame. */
            NavResult addproduct_nav = (cat_dropdown_open || unit_dropdown_open)
                ? (NavResult){ false, false }
                : nav_handle_focus(edit_flags, 5);

            float bx = box.x + 20, by = box.y + 40;

            GuiLabel((Rectangle){ bx, by, 100, 24 }, "SKU");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, f_sku, sizeof f_sku, edit_sku) && !addproduct_nav.moved) {
                edit_sku = !edit_sku;
                if (edit_sku) { edit_name = edit_price = edit_threshold = edit_initial_qty = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Nom");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, f_name, sizeof f_name, edit_name) && !addproduct_nav.moved) {
                edit_name = !edit_name;
                if (edit_name) { edit_sku = edit_price = edit_threshold = edit_initial_qty = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Categorie");
            cat_field_rect = (Rectangle){ bx + 110, by, 280, 24 };
            bool cat_hover = CheckCollisionPointRec(GetMousePosition(), cat_field_rect);
            DrawRectangleRec(cat_field_rect, WHITE);
            DrawRectangleLinesEx(cat_field_rect, 1, cat_dropdown_open ? COLOR_ACCENT_BLUE : COLOR_BORDER);
            const char *cat_display = f_category[0] ? f_category : "Choisir une categorie";
            AppText(cat_display, cat_field_rect.x + 8, cat_field_rect.y + 5, 14,
                     f_category[0] ? (Color){30,41,59,255} : COLOR_TEXT_MUTED);
            AppText(cat_dropdown_open ? "^" : "v",
                     cat_field_rect.x + cat_field_rect.width - 18, cat_field_rect.y + 5, 14, COLOR_TEXT_MUTED);
                       if (cat_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                cat_dropdown_open = !cat_dropdown_open;
                cat_adding_new = false;
                unit_dropdown_open = false;
                edit_sku = edit_name = edit_price = edit_threshold = edit_initial_qty = false;
            }

            by += 44;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Unite");
            unit_field_rect = (Rectangle){ bx + 110, by, 280, 24 };
            bool unit_hover = CheckCollisionPointRec(GetMousePosition(), unit_field_rect);
            DrawRectangleRec(unit_field_rect, WHITE);
            DrawRectangleLinesEx(unit_field_rect, 1, unit_dropdown_open ? COLOR_ACCENT_BLUE : COLOR_BORDER);
            AppText(f_unit, unit_field_rect.x + 8, unit_field_rect.y + 5, 14, (Color){30,41,59,255});
            AppText(unit_dropdown_open ? "^" : "v",
                     unit_field_rect.x + unit_field_rect.width - 18, unit_field_rect.y + 5, 14, COLOR_TEXT_MUTED);
            if (unit_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                unit_dropdown_open = !unit_dropdown_open;
                cat_dropdown_open = false;
                cat_adding_new = false;
                edit_sku = edit_name = edit_price = edit_threshold = edit_initial_qty = false;
            }

            by += 44;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Prix unitaire");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, f_price, sizeof f_price, edit_price) && !addproduct_nav.moved) {
                edit_price = !edit_price;
                if (edit_price) { edit_sku = edit_name = edit_threshold = edit_initial_qty = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Seuil alerte");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, f_threshold, sizeof f_threshold, edit_threshold) && !addproduct_nav.moved) {
                edit_threshold = !edit_threshold;
                if (edit_threshold) { edit_sku = edit_name = edit_price = edit_initial_qty = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Quantite initiale");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, f_initial_qty, sizeof f_initial_qty, edit_initial_qty) && !addproduct_nav.moved) {
                edit_initial_qty = !edit_initial_qty;
                if (edit_initial_qty) { edit_sku = edit_name = edit_price = edit_threshold = false; }
            }

            by += 50;

            /* Guard: while the category or unit dropdown is open, don't let
               clicks "leak through" to Enregistrer/Annuler underneath - the
               dropdown list is drawn on top of this area, so a click on a
               dropdown row would otherwise ALSO register on whichever
               button happens to sit at that same pixel this frame. */
            if (!cat_dropdown_open && !unit_dropdown_open) {
                if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Enregistrer") || addproduct_nav.submit) {
                    Product np = {0};
                    snprintf(np.sku, sizeof np.sku, "%s", f_sku);
                    snprintf(np.name, sizeof np.name, "%s", f_name);
                    snprintf(np.category, sizeof np.category, "%s", f_category);
                    snprintf(np.unit, sizeof np.unit, "%s", f_unit);
                    np.unit_price = (float)atof(f_price);
                    np.alert_threshold = atoi(f_threshold);
                    np.category_id = f_category_id;

                    char err[256];
                    if (!validate_product_fields(f_sku, f_name, f_price, f_threshold,
                                                  f_initial_qty, err, sizeof err)) {
                        toast_show(&toast, err, true);
                    } else if (inv_add_product(&np, err, sizeof err)) {
                        total_products = inv_all_products(&all_products);
                        inv_refresh_categories(db);
                        total_categories = inv_get_categories(&all_categories);

                        int initial_qty = atoi(f_initial_qty);
                        if (initial_qty > 0) {
                            Product *created = inv_find_by_sku(np.sku);
                            if (created) {
                                char mv_err[256];
                                inv_post_movement(created->id, 1, initial_qty, MV_RECEPTION,
                                                   "STOCK-INITIAL", &g_session,
                                                   "Stock de depart a la creation", mv_err, sizeof mv_err);
                                total_products = inv_all_products(&all_products);
                            }
                        }
                        toast_show(&toast, "Produit ajoute", false);
                        panel = PANEL_NONE;
                    } else {
                        toast_show(&toast, err, true);
                    }
                }
                if (GuiButton((Rectangle){ bx + 150, by, 130, 32 }, "Annuler")) panel = PANEL_NONE;
            }

            /* Category dropdown - drawn LAST so it always renders on top of
               every field below it (Prix unitaire, Seuil alerte, etc.)
               instead of getting clipped underneath them. */
            if (cat_dropdown_open) {
                float row_h = 26;
                int list_rows = total_categories + 1; /* +1 for "Ajouter" row */
                Rectangle dd = { cat_field_rect.x, cat_field_rect.y + cat_field_rect.height + 2,
                                  cat_field_rect.width, row_h * (cat_adding_new ? 2.4f : (float)list_rows) };
                DrawRectangleRec(dd, WHITE);
                DrawRectangleLinesEx(dd, 1, COLOR_BORDER);

                if (!cat_adding_new) {
                    Rectangle add_row = { dd.x, dd.y, dd.width, row_h };
                    bool add_hover = CheckCollisionPointRec(GetMousePosition(), add_row);
                    if (add_hover) DrawRectangleRec(add_row, (Color){239,246,255,255});
                    AppText("+ Ajouter nouvelle categorie", add_row.x + 8, add_row.y + 5, 14, COLOR_ACCENT_BLUE);
                                        if (add_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        cat_adding_new = true;
                        f_new_cat_input[0] = '\0';
                        edit_new_cat = true;
                        edit_sku = edit_name = edit_price = edit_threshold = edit_initial_qty = false;
                    }

                    for (int ci = 0; ci < total_categories; ci++) {
                        Rectangle row = { dd.x, dd.y + (ci + 1) * row_h, dd.width, row_h };
                        bool hover = CheckCollisionPointRec(GetMousePosition(), row);
                        bool selected = (f_category_id == all_categories[ci].id);
                        if (hover || selected) DrawRectangleRec(row, (Color){239,246,255,255});
                        AppText(all_categories[ci].name, row.x + 8, row.y + 5, 14, (Color){30,41,59,255});
                        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            snprintf(f_category, sizeof f_category, "%s", all_categories[ci].name);
                            f_category_id = all_categories[ci].id;
                            cat_dropdown_open = false;
                        }
                    }
                } else {
                    Rectangle input_row = { dd.x + 6, dd.y + 6, dd.width - 12, 24 };
                    GuiTextBox(input_row, f_new_cat_input, sizeof f_new_cat_input, edit_new_cat);

                    Rectangle confirm_btn = { dd.x + 6, dd.y + 36, (dd.width - 18) / 2, 24 };
                    Rectangle cancel_btn  = { confirm_btn.x + confirm_btn.width + 6, dd.y + 36, confirm_btn.width, 24 };

                    bool confirm_hover = CheckCollisionPointRec(GetMousePosition(), confirm_btn);
                    DrawRectangleRounded(confirm_btn, 0.2f, 4, confirm_hover ? (Color){29,78,216,255} : COLOR_ACCENT_BLUE);
                    AppText("Valider", confirm_btn.x + 10, confirm_btn.y + 4, 13, WHITE);

                    bool cancel_hover = CheckCollisionPointRec(GetMousePosition(), cancel_btn);
                    DrawRectangleLinesEx(cancel_btn, 1, COLOR_BORDER);
                    AppText("Annuler", cancel_btn.x + 12, cancel_btn.y + 4, 13, COLOR_TEXT_MUTED);

                    bool confirm_click = (confirm_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ||
                                          (edit_new_cat && IsKeyPressed(KEY_ENTER));
                    if (confirm_click) {
                        if (f_new_cat_input[0] != '\0') {
                            int new_id = 0;
                            if (db_find_or_create_category(db, f_new_cat_input, &new_id)) {
                                inv_refresh_categories(db);
                                total_categories = inv_get_categories(&all_categories);
                                snprintf(f_category, sizeof f_category, "%s", f_new_cat_input);
                                f_category_id = new_id;
                            }
                        }
                        cat_adding_new = false;
                        cat_dropdown_open = false;
                    }
                                        if (cancel_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        cat_adding_new = false;
                    }
                }
            }

            if (unit_dropdown_open) {
                float row_h = 26;
                Rectangle dd = { unit_field_rect.x, unit_field_rect.y + unit_field_rect.height + 2,
                                  unit_field_rect.width, row_h * UNIT_OPTION_COUNT };
                DrawRectangleRec(dd, WHITE);
                DrawRectangleLinesEx(dd, 1, COLOR_BORDER);
                for (int ui = 0; ui < UNIT_OPTION_COUNT; ui++) {
                    Rectangle row = { dd.x, dd.y + ui * row_h, dd.width, row_h };
                    bool hover = CheckCollisionPointRec(GetMousePosition(), row);
                    bool selected = (strcmp(f_unit, UNIT_OPTIONS[ui]) == 0);
                    if (hover || selected) DrawRectangleRec(row, (Color){239,246,255,255});
                    AppText(UNIT_OPTIONS[ui], row.x + 8, row.y + 5, 14, (Color){30,41,59,255});
                    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        snprintf(f_unit, sizeof f_unit, "%s", UNIT_OPTIONS[ui]);
                        unit_dropdown_open = false;
                    }
                }
            }
        }

                /* ---- Edit product panel ---- */
        if (panel == PANEL_EDIT_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 235, 440, 470 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Modifier le produit");

            bool *e_flags[3] = { &ee_name, &ee_price, &ee_threshold };
            bool e_any = ee_name || ee_price || ee_threshold || e_cat_dropdown_open || e_unit_dropdown_open;
            if (!e_any) ee_name = true;
            NavResult edit_nav = (e_cat_dropdown_open || e_unit_dropdown_open)
                ? (NavResult){ false, false }
                : nav_handle_focus(e_flags, 3);

            float bx = box.x + 20, by = box.y + 40;

            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Nom");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, e_name, sizeof e_name, ee_name) && !edit_nav.moved) {
                ee_name = !ee_name;
                if (ee_name) { ee_price = ee_threshold = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Categorie");
            e_cat_field_rect = (Rectangle){ bx + 110, by, 280, 24 };
            bool e_cat_hover = CheckCollisionPointRec(GetMousePosition(), e_cat_field_rect);
            DrawRectangleRec(e_cat_field_rect, WHITE);
            DrawRectangleLinesEx(e_cat_field_rect, 1, e_cat_dropdown_open ? COLOR_ACCENT_BLUE : COLOR_BORDER);
            const char *e_cat_display = e_category[0] ? e_category : "Choisir une categorie";
            AppText(e_cat_display, e_cat_field_rect.x + 8, e_cat_field_rect.y + 5, 14,
                     e_category[0] ? (Color){30,41,59,255} : COLOR_TEXT_MUTED);
            AppText(e_cat_dropdown_open ? "^" : "v",
                     e_cat_field_rect.x + e_cat_field_rect.width - 18, e_cat_field_rect.y + 5, 14, COLOR_TEXT_MUTED);
                        if (e_cat_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                e_cat_dropdown_open = !e_cat_dropdown_open;
                e_unit_dropdown_open = false;
                ee_name = ee_price = ee_threshold = false;
            }

            by += 44;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Unite");
            e_unit_field_rect = (Rectangle){ bx + 110, by, 280, 24 };
            bool e_unit_hover = CheckCollisionPointRec(GetMousePosition(), e_unit_field_rect);
            DrawRectangleRec(e_unit_field_rect, WHITE);
            DrawRectangleLinesEx(e_unit_field_rect, 1, e_unit_dropdown_open ? COLOR_ACCENT_BLUE : COLOR_BORDER);
            AppText(e_unit, e_unit_field_rect.x + 8, e_unit_field_rect.y + 5, 14, (Color){30,41,59,255});
            AppText(e_unit_dropdown_open ? "^" : "v",
                     e_unit_field_rect.x + e_unit_field_rect.width - 18, e_unit_field_rect.y + 5, 14, COLOR_TEXT_MUTED);
            if (e_unit_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                e_unit_dropdown_open = !e_unit_dropdown_open;
                e_cat_dropdown_open = false;
                ee_name = ee_price = ee_threshold = false;
            }

            by += 44;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Prix unitaire");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, e_price, sizeof e_price, ee_price) && !edit_nav.moved) {
                ee_price = !ee_price;
                if (ee_price) { ee_name = ee_threshold = false; }
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Seuil alerte");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, e_threshold, sizeof e_threshold, ee_threshold) && !edit_nav.moved) {
                ee_threshold = !ee_threshold;
                if (ee_threshold) { ee_name = ee_price = false; }
            }

            by += 50;
            if (!e_cat_dropdown_open && !e_unit_dropdown_open) {
                if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Enregistrer") || edit_nav.submit) {
                    Product upd = {0};
                    upd.id = edit_product_id;
                    upd.version = edit_product_version;
                    snprintf(upd.name, sizeof upd.name, "%s", e_name);
                    snprintf(upd.category, sizeof upd.category, "%s", e_category);
                    upd.category_id = e_category_id;
                    snprintf(upd.unit, sizeof upd.unit, "%s", e_unit);
                    upd.unit_price = (float)atof(e_price);
                    upd.alert_threshold = atoi(e_threshold);

                    char err[256];
                    if (!validate_product_fields(NULL, e_name, e_price, e_threshold,
                                                  NULL, err, sizeof err)) {
                        toast_show(&toast, err, true);
                    } else if (inv_update_product(&upd, err, sizeof err)) {
                        total_products = inv_all_products(&all_products);
                        toast_show(&toast, "Produit modifie", false);
                        panel = PANEL_NONE;
                    } else {
                        toast_show(&toast, err, true); /* e.g. version conflict message */
                    }
                }
                if (GuiButton((Rectangle){ bx + 150, by, 130, 32 }, "Annuler")) panel = PANEL_NONE;
            }

            if (e_cat_dropdown_open) {
                float row_h = 26;
                Rectangle dd = { e_cat_field_rect.x, e_cat_field_rect.y + e_cat_field_rect.height + 2,
                                  e_cat_field_rect.width, row_h * (float)total_categories };
                DrawRectangleRec(dd, WHITE);
                DrawRectangleLinesEx(dd, 1, COLOR_BORDER);
                for (int ci = 0; ci < total_categories; ci++) {
                    Rectangle row = { dd.x, dd.y + ci * row_h, dd.width, row_h };
                    bool hover = CheckCollisionPointRec(GetMousePosition(), row);
                    bool selected = (e_category_id == all_categories[ci].id);
                    if (hover || selected) DrawRectangleRec(row, (Color){239,246,255,255});
                    AppText(all_categories[ci].name, row.x + 8, row.y + 5, 14, (Color){30,41,59,255});
                                        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        snprintf(e_category, sizeof e_category, "%s", all_categories[ci].name);
                        e_category_id = all_categories[ci].id;
                        e_cat_dropdown_open = false;
                    }
                }
            }

            if (e_unit_dropdown_open) {
                float row_h = 26;
                Rectangle dd = { e_unit_field_rect.x, e_unit_field_rect.y + e_unit_field_rect.height + 2,
                                  e_unit_field_rect.width, row_h * UNIT_OPTION_COUNT };
                DrawRectangleRec(dd, WHITE);
                DrawRectangleLinesEx(dd, 1, COLOR_BORDER);
                for (int ui = 0; ui < UNIT_OPTION_COUNT; ui++) {
                    Rectangle row = { dd.x, dd.y + ui * row_h, dd.width, row_h };
                    bool hover = CheckCollisionPointRec(GetMousePosition(), row);
                    bool selected = (strcmp(e_unit, UNIT_OPTIONS[ui]) == 0);
                    if (hover || selected) DrawRectangleRec(row, (Color){239,246,255,255});
                    AppText(UNIT_OPTIONS[ui], row.x + 8, row.y + 5, 14, (Color){30,41,59,255});
                    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        snprintf(e_unit, sizeof e_unit, "%s", UNIT_OPTIONS[ui]);
                        e_unit_dropdown_open = false;
                    }
                }
            }
        }

        /* ---- Confirm delete product ---- */
        if (panel == PANEL_CONFIRM_DELETE_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 90, 400, 180 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Confirmer l'archivage");

            float bx = box.x + 20, by = box.y + 40;
            AppText("Voulez-vous archiver ce produit ?", bx, by, 14, (Color){30,41,59,255});
            AppText("Il restera consultable et restaurable depuis \"Produits archives\".", bx, by + 22, 13, COLOR_TEXT_MUTED);

            by += 60;
            if (GuiButton((Rectangle){ bx, by, 160, 34 }, "Oui, archiver")) {
                char err[256];
                if (inv_delete_product(edit_product_id, &g_session, err, sizeof err)) {
                    total_products = inv_all_products(&all_products);
                    selected_product_id = -1;
                    toast_show(&toast, "Produit archive", false);
                } else {
                    toast_show(&toast, err, true); /* e.g. "stock restant non nul" */
                }
                panel = PANEL_NONE;
            }
            if (GuiButton((Rectangle){ bx + 180, by, 160, 34 }, "Annuler")) panel = PANEL_NONE;
        }

        /* ---- Manage categories panel ---- */
                if (panel == PANEL_MANAGE_CATEGORIES) {
            float list_h = 30.0f * (total_categories > 0 ? total_categories : 1) + 100;
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - list_h/2, 440, list_h };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Gerer les categories");

            float bx = box.x + 20, by = box.y + 40;
            if (total_categories == 0) {
                AppText("Aucune categorie pour le moment.", bx, by, 14, COLOR_TEXT_MUTED);
                by += 30;
            }

            for (int ci = 0; ci < total_categories; ci++) {
                AppText(all_categories[ci].name, bx, by + 4, 14, (Color){30,41,59,255});
                if (GuiButton((Rectangle){ box.x + box.width - 170, by, 70, 26 }, "Modifier")) {
                    rename_category_id = all_categories[ci].id;
                    snprintf(rename_category_input, sizeof rename_category_input, "%s", all_categories[ci].name);
                    rename_category_edit = true;
                    panel = PANEL_RENAME_CATEGORY;
                }
                if (GuiButton((Rectangle){ box.x + box.width - 90, by, 70, 26 }, "Suppr.")) {
                    delete_category_id = all_categories[ci].id;
                    snprintf(delete_category_name, sizeof delete_category_name, "%s", all_categories[ci].name);
                    panel = PANEL_CONFIRM_DELETE_CATEGORY;
                }
                by += 30;
            }

            /* "Fermer" pinned to the bottom-left of the panel, with a
               consistent gap - not drifting based on list length. */
            if (GuiButton((Rectangle){ bx, box.y + box.height - 50, 130, 32 }, "Fermer")) panel = PANEL_NONE;
        }

        /* ---- Confirm delete category ---- */
        if (panel == PANEL_CONFIRM_DELETE_CATEGORY) {
            Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 90, 400, 180 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Confirmer la suppression");

            float bx = box.x + 20, by = box.y + 40;
            AppText(TextFormat("Supprimer la categorie \"%s\" ?", delete_category_name), bx, by, 14, (Color){30,41,59,255});

            by += 40;
            if (GuiButton((Rectangle){ bx, by, 160, 34 }, "Oui, supprimer")) {
                char err[256];
                if (inv_delete_category(delete_category_id, &g_session, err, sizeof err)) {
                    total_categories = inv_get_categories(&all_categories);
                    toast_show(&toast, "Categorie supprimee", false);
                    panel = PANEL_MANAGE_CATEGORIES;
                } else {
                    toast_show(&toast, err, true); /* cascade-safety message */
                    panel = PANEL_MANAGE_CATEGORIES;
                }
            }
            if (GuiButton((Rectangle){ bx + 180, by, 160, 34 }, "Annuler")) panel = PANEL_MANAGE_CATEGORIES;
        }

        /* ---- Rename category (from "Gerer categories" panel) ---- */
        if (panel == PANEL_RENAME_CATEGORY) {
            Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 80, 400, 160 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Renommer la categorie");
            float bx = box.x + 20, by = box.y + 40;
            if (!rename_category_edit) rename_category_edit = true;
            GuiTextBox((Rectangle){ bx, by, box.width - 40, 30 }, rename_category_input, 64, rename_category_edit);

            by += 46;
            bool confirm = GuiButton((Rectangle){ bx, by, 150, 32 }, "Enregistrer") ||
                           (rename_category_edit && IsKeyPressed(KEY_ENTER));
            if (confirm) {
                if (rename_category_input[0] != '\0' &&
                    db_update_category_name(db, rename_category_id, rename_category_input)) {
                    inv_refresh_categories(db);
                    total_categories = inv_get_categories(&all_categories);
                    toast_show(&toast, "Categorie renommee", false);
                } else {
                    toast_show(&toast, "Erreur lors du renommage", true);
                }
                panel = PANEL_MANAGE_CATEGORIES;
            }
            if (GuiButton((Rectangle){ bx + 160, by, 150, 32 }, "Annuler")) panel = PANEL_MANAGE_CATEGORIES;
        }

                /* ---- Manage users panel (admin only) ---- */
        if (panel == PANEL_MANAGE_USERS) {
            float list_h = 36.0f * (mgmt_user_count > 0 ? mgmt_user_count : 1) + 90;
            Rectangle box = { GetScreenWidth()/2 - 260, GetScreenHeight()/2 - list_h/2, 520, list_h };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Gerer les utilisateurs");

            float bx = box.x + 20, by = box.y + 40;
            const char *roles[3] = { "operateur", "manager", "admin" };

            for (int ui = 0; ui < mgmt_user_count; ui++) {
                AppText(mgmt_user_names[ui], bx, by + 6, 14, (Color){30,41,59,255});

                /* Three small role buttons - click to set that user's role.
                   Current role shown highlighted, matching the earlier
                   "[Entree]"/"Entree" toggle-button pattern already used
                   in the movement panel. */
                for (int ri = 0; ri < 3; ri++) {
                    bool is_current = strcmp(mgmt_user_roles[ui], roles[ri]) == 0;
                    Rectangle rbtn = { box.x + box.width - 300 + ri * 100, by, 90, 28 };
                    bool hover = CheckCollisionPointRec(GetMousePosition(), rbtn);
                    DrawRectangleRec(rbtn, is_current ? COLOR_ACCENT_BLUE :
                                             (hover ? (Color){239,246,255,255} : WHITE));
                    DrawRectangleLinesEx(rbtn, 1, COLOR_BORDER);
                    AppText(roles[ri], rbtn.x + 6, rbtn.y + 6, 12,
                             is_current ? WHITE : (Color){30,41,59,255});

                    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !is_current) {
                        if (db_update_user_role(db, mgmt_user_ids[ui], roles[ri])) {
                            snprintf(mgmt_user_roles[ui], 16, "%s", roles[ri]);
                            toast_show(&toast, "Role mis a jour", false);
                        } else {
                            toast_show(&toast, "Erreur lors de la mise a jour", true);
                        }
                    }
                }
                by += 36;
            }

            if (GuiButton((Rectangle){ bx, box.y + box.height - 50, 130, 32 }, "Fermer")) panel = PANEL_NONE;
        }

        /* ---- Movement panel ---- */
        Product *movement_product = find_product_by_id(all_products, total_products, selected_product_id);
        if (panel == PANEL_MOVEMENT && movement_product != NULL) {
            Product *p = movement_product;
                       Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 165, 400, 320 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, TextFormat("Mouvement - %s", p->name));

            float bx = box.x + 20, by = box.y + 40;
            DrawText(TextFormat("Stock actuel: %d %s", p->total_quantity, p->unit), bx, by, 14, DARKGRAY);
            by += 30;

            if (GuiButton((Rectangle){ bx, by, 90, 30 }, movement_sign > 0 ? "[Entree]" : "Entree"))
                movement_sign = 1;
            if (GuiButton((Rectangle){ bx + 100, by, 90, 30 }, movement_sign < 0 ? "[Sortie]" : "Sortie"))
                movement_sign = -1;

                        if (!edit_qty && !edit_reason) edit_qty = true; /* default focus */
            bool *movement_fields[2] = { &edit_qty, &edit_reason };
            NavResult movement_nav = nav_handle_focus(movement_fields, 2);

            by += 40;
            GuiLabel((Rectangle){ bx, by, 90, 24 }, "Quantite");
            if (GuiTextBox((Rectangle){ bx + 100, by, 100, 24 }, m_qty, sizeof m_qty, edit_qty) && !movement_nav.moved) {
                edit_qty = !edit_qty;
                if (edit_qty) edit_reason = false;
            }

            by += 34;
            GuiLabel((Rectangle){ bx, by, 90, 24 }, "Raison");
            if (GuiTextBox((Rectangle){ bx + 100, by, 220, 24 }, m_reason, sizeof m_reason, edit_reason) && !movement_nav.moved) {
                edit_reason = !edit_reason;
                if (edit_reason) edit_qty = false;
            }

            by += 50;
            if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Valider") || movement_nav.submit) {
                if (!str_is_integer(m_qty) || atoi(m_qty) <= 0) {
                    toast_show(&toast, "Quantite invalide (nombre entier positif requis)", true);
                } else {
                    int qty = atoi(m_qty);
                    /* NOTE: location_id=1 placeholder - a real build wires
                       this to a location picker; the atomic-update logic
                       itself (inv_post_movement) is what matters here. */
                    char err[256];
                    bool ok = inv_post_movement(p->id, 1, movement_sign * qty,
                                movement_sign > 0 ? MV_RECEPTION : MV_EXPEDITION,
                                "GUI", &g_session, m_reason, err, sizeof err);
                    if (ok) {
                        toast_show(&toast, "Mouvement enregistre", false);
                        panel = PANEL_NONE;
                    } else {
                        toast_show(&toast, err, true);
                    }
                }
            }
                       if (GuiButton((Rectangle){ bx + 150, by, 130, 32 }, "Annuler")) panel = PANEL_NONE;
        }

        /* ---- Movement history panel ---- */
                if (panel == PANEL_MOVEMENT_HISTORY) {
            int rows = mv_history_count > 12 ? 12 : mv_history_count;
            float list_h = 28.0f * (rows > 0 ? rows : 1) + 110;
            int box_w = 820;
            if (box_w > GetScreenWidth() - 40) box_w = GetScreenWidth() - 40;
            Rectangle box = { GetScreenWidth()/2 - box_w/2.0f, GetScreenHeight()/2 - list_h/2, (float)box_w, list_h };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            Product *hist_p = find_product_by_id(all_products, total_products, selected_product_id);
            GuiPanel(box, TextFormat("Historique des mouvements - %s", hist_p ? hist_p->name : ""));

            float bx = box.x + 20, by = box.y + 40;
            float col_date = bx, col_qty = bx + 150, col_type = bx + 210,
                  col_user = bx + 330, col_ref = bx + 460;
            float ref_w = (box.x + box.width - 20) - col_ref; /* whatever room is left - always inside the box */

            AppText("Date/heure", col_date, by, 12, DARKGRAY);
            AppText("Qte", col_qty, by, 12, DARKGRAY);
            AppText("Type", col_type, by, 12, DARKGRAY);
            AppText("Utilisateur", col_user, by, 12, DARKGRAY);
            AppText("Reference / raison", col_ref, by, 12, DARKGRAY);
            by += 22;

            if (mv_history_count == 0) {
                AppText("Aucun mouvement enregistre pour ce produit.", bx, by, 13, COLOR_TEXT_MUTED);
                by += 26;
            }

            for (int mi = 0; mi < rows; mi++) {
                Movement *mv = &mv_history[mi];
                AppText(mv->created_at, col_date, by, 13, (Color){30,41,59,255});

                char delta_buf[16];
                snprintf(delta_buf, sizeof delta_buf, "%+d", mv->delta);
                AppText(delta_buf, col_qty, by, 13, mv->delta >= 0 ? COLOR_ACCENT_TEAL : COLOR_ACCENT_RED);

                AppText(mv->type, col_type, by, 13, (Color){30,41,59,255});
                AppText(mv->username[0] ? mv->username : "-", col_user, by, 13, COLOR_TEXT_MUTED);

                const char *detail = mv->reason[0] ? mv->reason : mv->reference;
                /* Hard-clipped to whatever room is actually left in the
                   box, so long reasons can never draw past the rectangle
                   no matter the box width. */
                BeginScissorMode((int)col_ref, (int)(by - 2), (int)ref_w, 18);
                AppText(detail, col_ref, by, 13, COLOR_TEXT_MUTED);
                EndScissorMode();

                by += 24;
            }

            if (mv_history_count > rows) {
                char more_buf[64];
                snprintf(more_buf, sizeof more_buf, "+ %d mouvement(s) plus ancien(s) non affiches",
                         mv_history_count - rows);
                AppText(more_buf, bx, by, 12, COLOR_TEXT_MUTED);
                by += 22;
            }

            if (GuiButton((Rectangle){ bx, box.y + box.height - 46, 130, 32 }, "Fermer")) panel = PANEL_NONE;
        }

        /* ---- toast ---- */
        if (toast.timer > 0) {
            Color c = toast.is_error ? (Color){ 200, 60, 60, 255 } : (Color){ 40, 150, 90, 255 };
            float alpha = toast.timer > 1.0f ? 1.0f : toast.timer; /* fade last second */
            Color faded = { c.r, c.g, c.b, (unsigned char)(255 * alpha) };
            int tw = MeasureText(toast.text, 16) + 24;
            DrawRectangle(GetScreenWidth()/2 - tw/2, 100, tw, 32, faded);
            DrawText(toast.text, GetScreenWidth()/2 - tw/2 + 12, 108, 16, RAYWHITE);
        }

        /* ---- global shortcuts ---- */
if (IsKeyPressed(KEY_F2)) {
    panel = PANEL_ADD_PRODUCT;
    f_sku[0] = f_name[0] = f_price[0] = f_threshold[0] = '\0';
    snprintf(f_unit, sizeof f_unit, "piece");
    unit_dropdown_open = false;
    if (g_active_category_id > 0) {
        f_category_id = g_active_category_id;
        snprintf(f_category, sizeof f_category, "%s", g_active_category_name);
    } else {
        f_category_id = 0;
        f_category[0] = '\0';
    }
}
        if (!modal_active && IsKeyPressed(KEY_F3)) search_edit = true;

        EndDrawing();
    }

    if (g_session.active) session_logout(db, &g_session);
    CloseWindow();
}
