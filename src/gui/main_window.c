#include "gui.h"
#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PAGE_SIZE 10
#define SCREEN_W  1024
#define SCREEN_H  768
#define COLOR_NAVY_DARK   (Color){ 15, 23, 42, 255 }     /* sidebar/header bg */
#define COLOR_NAVY_MID    (Color){ 30, 41, 59, 255 }      /* panel accents */
#define COLOR_ACCENT_BLUE (Color){ 37, 99, 235, 255 }     /* primary buttons */
#define COLOR_ACCENT_TEAL (Color){ 16, 185, 129, 255 }    /* success/positive */
#define COLOR_ACCENT_RED  (Color){ 220, 38, 38, 255 }     /* errors/alerts */
#define COLOR_BG_LIGHT    (Color){ 248, 250, 252, 255 }   /* main content bg */
#define COLOR_TEXT_MUTED  (Color){ 100, 116, 139, 255 }   /* secondary text */
#define COLOR_BORDER      (Color){ 226, 232, 240, 255 }   /* card borders */

#include "session.h"

typedef enum { SCREEN_SPLASH, SCREEN_LOGIN, SCREEN_MAIN } Screen;
static Screen  g_screen = SCREEN_SPLASH;
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
                g_screen = SCREEN_MAIN;
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
                    g_screen = SCREEN_MAIN;
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
               PANEL_MANAGE_CATEGORIES, PANEL_CONFIRM_DELETE_CATEGORY } ActivePanel;

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

    int selected_index = -1; /* index into whichever list is showing */

    ActivePanel panel = PANEL_NONE;
    Toast toast = {0};

           /* Add-product form fields */
    char f_sku[64] = {0}, f_name[128] = {0}, f_category[64] = {0};
    char f_price[32] = {0}, f_threshold[16] = {0}, f_initial_qty[16] = {0};
    bool edit_sku = false, edit_name = false, edit_cat = false,
         edit_price = false, edit_threshold = false, edit_initial_qty = false;

        /* Category select-dropdown state */
    int  f_category_id = 0;         /* 0 = none chosen yet */
    bool cat_dropdown_open = false;
    bool cat_adding_new = false;
    char f_new_cat_input[64] = {0};
    bool edit_new_cat = false;
    Rectangle cat_field_rect = {0};  /* filled in when the field is drawn, read by the overlay at the end */

    Category *all_categories;
    int total_categories = inv_get_categories(&all_categories);

    /* Movement form fields */
    char m_qty[16] = {0};
    bool edit_qty = false;
    int  movement_sign = 1; /* +1 in, -1 out */

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

    /* Category management panel */
    int delete_category_id = 0;
    char delete_category_name[64] = {0};

    while (!WindowShouldClose()) {

        /* ---- session bookkeeping (runs every frame, before anything else) ---- */
        if (g_session.active &&
            (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
            session_touch(&g_session);
        }
        if (g_screen == SCREEN_MAIN && session_is_locked(&g_session, 300)) {
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

        /* ---- update (main screen only, reached once logged in) ---- */
        if (toast.timer > 0) toast.timer -= GetFrameTime();

        if (strlen(search_query) > 0) {
            filtered_count = inv_search(search_query, filtered, WMS_MAX_PRODUCTS);
            is_filtering = true;
        } else {
            is_filtering = false;
            filtered_count = 0;
        }

        Product **visible_list = is_filtering ? filtered : NULL;
        (void)visible_list;
        int visible_count = is_filtering ? filtered_count : total_products;
        int page_count = (visible_count + PAGE_SIZE - 1) / PAGE_SIZE;
        if (page >= page_count) page = page_count > 0 ? page_count - 1 : 0;

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

                char subtitle[64];
        snprintf(subtitle, sizeof subtitle, "%d produits", total_products);
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

        if (GuiButton((Rectangle){ tx, toolbar_y, 160, sh }, "+ Nouveau produit")) {
            panel = PANEL_ADD_PRODUCT;
            f_sku[0] = f_name[0] = f_category[0] = f_price[0] = f_threshold[0] = f_initial_qty[0] = '\0';
            f_category_id = 0; cat_dropdown_open = false; cat_adding_new = false; f_new_cat_input[0] = '\0';
        }
        tx += 160 + btn_gap;

        if (GuiButton((Rectangle){ tx, toolbar_y, 140, sh }, "Mouvement stock")) {
            if (selected_index >= 0) { panel = PANEL_MOVEMENT; m_qty[0] = '\0'; }
            else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
        tx += 140 + btn_gap;

        if (GuiButton((Rectangle){ tx, toolbar_y, 100, sh }, "Modifier")) {
            if (selected_index >= 0) {
                Product *p = is_filtering ? filtered[selected_index] : &all_products[selected_index];
                edit_product_id = p->id;
                edit_product_version = p->version;
                snprintf(e_name, sizeof e_name, "%s", p->name);
                snprintf(e_price, sizeof e_price, "%.2f", p->unit_price);
                snprintf(e_threshold, sizeof e_threshold, "%d", p->alert_threshold);
                e_category_id = p->category_id;
                snprintf(e_category, sizeof e_category, "%s", p->category);
                panel = PANEL_EDIT_PRODUCT;
                ee_name = true; ee_price = ee_threshold = false;
            } else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
        tx += 100 + btn_gap;

        if (GuiButton((Rectangle){ tx, toolbar_y, 110, sh }, "Supprimer")) {
            if (selected_index >= 0) {
                Product *p = is_filtering ? filtered[selected_index] : &all_products[selected_index];
                edit_product_id = p->id;
                panel = PANEL_CONFIRM_DELETE_PRODUCT;
            } else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }
        tx += 110 + btn_gap;

        if (GuiButton((Rectangle){ tx, toolbar_y, 150, sh }, "Gerer categories")) {
            panel = PANEL_MANAGE_CATEGORIES;
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
            Product *p = is_filtering ? filtered[i] : &all_products[i];
            Rectangle row_rect = { table_x - 4, row_y - 4, GetScreenWidth() - 2 * table_x + 8, 26 };
            bool hovered = CheckCollisionPointRec(GetMousePosition(), row_rect);
            bool selected = (selected_index == i);

            if (selected) DrawRectangleRec(row_rect, (Color){ 210, 225, 245, 255 });
            else if (hovered) DrawRectangleRec(row_rect, (Color){ 235, 240, 248, 255 });

            if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) selected_index = i;

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
            row_y += 30;
        }

        /* Pagination controls */
        int pag_y = GetScreenHeight() - 60;
        char page_label[32];
        snprintf(page_label, sizeof page_label, "Page %d / %d", page + 1, page_count > 0 ? page_count : 1);
        DrawText(page_label, table_x, pag_y + 6, 14, DARKGRAY);
        if (GuiButton((Rectangle){ table_x + 140, pag_y, 70, 30 }, "< Prec") && page > 0) page--;
        if (GuiButton((Rectangle){ table_x + 220, pag_y, 70, 30 }, "Suiv >") && page + 1 < page_count) page++;
        /* ---- Add product panel (modal-ish) ---- */
                if (panel == PANEL_ADD_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 225, 440, 470 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Nouveau produit");

                       bool *edit_flags[5] = { &edit_sku, &edit_name,
                                     &edit_price, &edit_threshold, &edit_initial_qty };
            bool any_focused = edit_sku || edit_name || edit_price ||
                                edit_threshold || edit_initial_qty ||
                                cat_dropdown_open || edit_new_cat;
            if (!any_focused) edit_sku = true;

            /* While the category dropdown/add-category box is active, the
               5-field Tab/↑↓ navigation must not run at all - otherwise it
               fights with the dropdown for focus every frame. */
            NavResult addproduct_nav = cat_dropdown_open
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
            /* Guard: while the category dropdown is open, don't let clicks
               "leak through" to Enregistrer/Annuler underneath - the
               dropdown list is drawn on top of this area, so a click on a
               category row would otherwise ALSO register on whichever
               button happens to sit at that same pixel this frame. */
            if (!cat_dropdown_open) {
                if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Enregistrer") || addproduct_nav.submit) {
                    Product np = {0};
                    snprintf(np.sku, sizeof np.sku, "%s", f_sku);
                    snprintf(np.name, sizeof np.name, "%s", f_name);
                    snprintf(np.category, sizeof np.category, "%s", f_category);
                    snprintf(np.unit, sizeof np.unit, "pcs");
                    np.unit_price = (float)atof(f_price);
                    np.alert_threshold = atoi(f_threshold);
                    np.category_id = f_category_id;

                    char err[256];
                    if (f_sku[0] == '\0' || f_name[0] == '\0') {
                        toast_show(&toast, "SKU et nom sont obligatoires", true);
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
        }

                /* ---- Edit product panel ---- */
        if (panel == PANEL_EDIT_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 210, 440, 420 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Modifier le produit");

            bool *e_flags[3] = { &ee_name, &ee_price, &ee_threshold };
            bool e_any = ee_name || ee_price || ee_threshold || e_cat_dropdown_open;
            if (!e_any) ee_name = true;
            NavResult edit_nav = e_cat_dropdown_open
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
            if (!e_cat_dropdown_open) {
                if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Enregistrer") || edit_nav.submit) {
                    Product upd = {0};
                    upd.id = edit_product_id;
                    upd.version = edit_product_version;
                    snprintf(upd.name, sizeof upd.name, "%s", e_name);
                    snprintf(upd.category, sizeof upd.category, "%s", e_category);
                    upd.category_id = e_category_id;
                    upd.unit_price = (float)atof(e_price);
                    upd.alert_threshold = atoi(e_threshold);

                    char err[256];
                    if (inv_update_product(&upd, err, sizeof err)) {
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
        }

        /* ---- Confirm delete product ---- */
        if (panel == PANEL_CONFIRM_DELETE_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 90, 400, 180 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Confirmer la suppression");

            float bx = box.x + 20, by = box.y + 40;
            AppText("Voulez-vous vraiment supprimer ce produit ?", bx, by, 14, (Color){30,41,59,255});
            AppText("Cette action est irreversible.", bx, by + 22, 13, COLOR_TEXT_MUTED);

            by += 60;
            if (GuiButton((Rectangle){ bx, by, 160, 34 }, "Oui, supprimer")) {
                char err[256];
                if (inv_delete_product(edit_product_id, &g_session, err, sizeof err)) {
                    total_products = inv_all_products(&all_products);
                    selected_index = -1;
                    toast_show(&toast, "Produit supprime", false);
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

        /* ---- Movement panel ---- */
        if (panel == PANEL_MOVEMENT && selected_index >= 0) {
            Product *p = is_filtering ? filtered[selected_index] : &all_products[selected_index];
            Rectangle box = { GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 140, 400, 260 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, TextFormat("Mouvement - %s", p->name));

            float bx = box.x + 20, by = box.y + 40;
            DrawText(TextFormat("Stock actuel: %d %s", p->total_quantity, p->unit), bx, by, 14, DARKGRAY);
            by += 30;

            if (GuiButton((Rectangle){ bx, by, 90, 30 }, movement_sign > 0 ? "[Entree]" : "Entree"))
                movement_sign = 1;
            if (GuiButton((Rectangle){ bx + 100, by, 90, 30 }, movement_sign < 0 ? "[Sortie]" : "Sortie"))
                movement_sign = -1;

                        if (!edit_qty) edit_qty = true; /* only one field - always focused */
            bool *movement_fields[1] = { &edit_qty };
            NavResult movement_nav = nav_handle_focus(movement_fields, 1);

            by += 40;
            GuiLabel((Rectangle){ bx, by, 90, 24 }, "Quantite");
            if (GuiTextBox((Rectangle){ bx + 100, by, 100, 24 }, m_qty, sizeof m_qty, edit_qty) && !movement_nav.moved)
                edit_qty = !edit_qty;

            by += 50;
            if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Valider") || movement_nav.submit) {
                int qty = atoi(m_qty);
                if (qty <= 0) {
                    toast_show(&toast, "Quantite invalide", true);
                } else {
                    /* NOTE: location_id=1 placeholder - a real build wires
                       this to a location picker; the atomic-update logic
                       itself (inv_post_movement) is what matters here. */
                    char err[256];
                    bool ok = inv_post_movement(p->id, 1, movement_sign * qty,
                                movement_sign > 0 ? MV_RECEPTION : MV_EXPEDITION,
                                "GUI", &g_session, NULL, err, sizeof err);
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
        if (IsKeyPressed(KEY_F2)) { panel = PANEL_ADD_PRODUCT; f_sku[0]=f_name[0]=f_category[0]=f_price[0]=f_threshold[0]='\0'; }
        if (IsKeyPressed(KEY_F3)) search_edit = true;

        EndDrawing();
    }

    if (g_session.active) session_logout(db, &g_session);
    CloseWindow();
}
