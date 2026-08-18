#include "gui.h"
#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PAGE_SIZE 10
#define SCREEN_W  1024
#define SCREEN_H  768

#include "session.h"

typedef enum { SCREEN_LOGIN, SCREEN_MAIN } Screen;
static Screen  g_screen = SCREEN_LOGIN;
static Session g_session = {0};

static char login_username[64] = "";
static char login_password[64] = "";
static bool username_edit = false;
static bool password_edit = false;
static char login_error[128] = "";
static float error_fade = 0.0f;

static void draw_login_screen(WmsDb *db) {

    ClearBackground((Color){ 22, 32, 48, 255 });
    int panel_w = 360, panel_h = 260;
    int px = (SCREEN_W - panel_w) / 2, py = (SCREEN_H - panel_h) / 2;

    GuiPanel((Rectangle){ px, py, panel_w, panel_h }, "SmartStock -- Connexion");
    GuiLabel((Rectangle){ px + 24, py + 50, 100, 24 }, "Utilisateur");
    if (GuiTextBox((Rectangle){ px + 24, py + 76, panel_w - 48, 32 },
                    login_username, sizeof(login_username), username_edit))
        username_edit = !username_edit;

    GuiLabel((Rectangle){ px + 24, py + 116, 100, 24 }, "Mot de passe");
    if (GuiTextBoxPassword((Rectangle){ px + 24, py + 142, panel_w - 48, 32 },
                            login_password, sizeof(login_password), password_edit))
        password_edit = !password_edit;

    bool submit = GuiButton((Rectangle){ px + 24, py + 190, panel_w - 48, 36 },
                             "Se connecter") || IsKeyPressed(KEY_ENTER);

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
        Color c = RED;
        c.a = (unsigned char)(error_fade * 255);
        DrawText(login_error, px + 24, py + 232, 16, c);
        error_fade -= GetFrameTime() * 0.5f;
    }
}

typedef enum { PANEL_NONE, PANEL_ADD_PRODUCT, PANEL_MOVEMENT } ActivePanel;

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

void gui_run(WmsDb *db) {
    InitWindow(SCREEN_W, SCREEN_H, "Gestion de Stock - Warehouse WMS");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(60);

    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);

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
    char f_price[32] = {0}, f_threshold[16] = {0};
    bool edit_sku = false, edit_name = false, edit_cat = false,
         edit_price = false, edit_threshold = false;

    /* Movement form fields */
    char m_qty[16] = {0};
    bool edit_qty = false;
    int  movement_sign = 1; /* +1 in, -1 out */

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
        DrawRectangle(0, 0, GetScreenWidth(), 64, (Color){ 21, 41, 71, 255 });
        DrawText("WAREHOUSE WMS", 20, 20, 24, RAYWHITE);
        char subtitle[64];
        snprintf(subtitle, sizeof subtitle, "%d produits", total_products);
        DrawText(subtitle, GetScreenWidth() - 160, 24, 16, (Color){180,190,210,255});

        /* Logged-in user + logout, per the login/session feature */
        DrawText(TextFormat("%s (%s)", g_session.username, g_session.role),
                  GetScreenWidth() - 320, 44, 14, (Color){180,190,210,255});
        if (GuiButton((Rectangle){ GetScreenWidth() - 100, 16, 80, 28 }, "Deconnexion")) {
            session_logout(db, &g_session);
            g_screen = SCREEN_LOGIN;
            login_username[0] = login_password[0] = '\0';
            EndDrawing();
            continue;
        }

        /* Search bar */
        int sx = 20, sy = 84, sw = 400, sh = 32;
        if (GuiTextBox((Rectangle){ sx, sy, sw, sh }, search_query, sizeof search_query, search_edit))
            search_edit = !search_edit;
        DrawText("F3: recherche", sx, sy + sh + 4, 12, GRAY);

        if (GuiButton((Rectangle){ sx + sw + 10, sy, 160, sh }, "+ Nouveau produit")) {
            panel = PANEL_ADD_PRODUCT;
            f_sku[0] = f_name[0] = f_category[0] = f_price[0] = f_threshold[0] = '\0';
        }
        if (GuiButton((Rectangle){ sx + sw + 180, sy, 140, sh }, "Mouvement stock")) {
            if (selected_index >= 0) { panel = PANEL_MOVEMENT; m_qty[0] = '\0'; }
            else toast_show(&toast, "Selectionnez un produit d'abord", true);
        }

        /* Table header */
        int tx = 20, ty = 140;
        int col_sku = tx, col_name = tx + 110, col_cat = tx + 380,
            col_qty = tx + 540, col_price = tx + 640, col_alert = tx + 760;
        DrawText("SKU", col_sku, ty, 14, DARKGRAY);
        DrawText("Nom", col_name, ty, 14, DARKGRAY);
        DrawText("Categorie", col_cat, ty, 14, DARKGRAY);
        DrawText("Qte", col_qty, ty, 14, DARKGRAY);
        DrawText("Prix", col_price, ty, 14, DARKGRAY);
        DrawText("Statut", col_alert, ty, 14, DARKGRAY);
        DrawLine(tx, ty + 22, GetScreenWidth() - 20, ty + 22, LIGHTGRAY);

        int row_y = ty + 32;
        int start = page * PAGE_SIZE;
        int shown_this_page = 0;

        for (int i = start; i < visible_count && shown_this_page < PAGE_SIZE; i++, shown_this_page++) {
            Product *p = is_filtering ? filtered[i] : &all_products[i];
            Rectangle row_rect = { tx - 4, row_y - 4, GetScreenWidth() - 2 * tx + 8, 26 };
            bool hovered = CheckCollisionPointRec(GetMousePosition(), row_rect);
            bool selected = (selected_index == i);

            if (selected) DrawRectangleRec(row_rect, (Color){ 210, 225, 245, 255 });
            else if (hovered) DrawRectangleRec(row_rect, (Color){ 235, 240, 248, 255 });

            if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) selected_index = i;

            DrawText(p->sku, col_sku, row_y, 14, BLACK);
            DrawText(p->name, col_name, row_y, 14, BLACK);
            DrawText(p->category, col_cat, row_y, 14, BLACK);

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
        DrawText(page_label, tx, pag_y + 6, 14, DARKGRAY);
        if (GuiButton((Rectangle){ tx + 140, pag_y, 70, 30 }, "< Prec") && page > 0) page--;
        if (GuiButton((Rectangle){ tx + 220, pag_y, 70, 30 }, "Suiv >") && page + 1 < page_count) page++;

        /* ---- Add product panel (modal-ish) ---- */
        if (panel == PANEL_ADD_PRODUCT) {
            Rectangle box = { GetScreenWidth()/2 - 220, GetScreenHeight()/2 - 200, 440, 400 };
            DrawRectangleRec((Rectangle){0,0,(float)GetScreenWidth(),(float)GetScreenHeight()}, (Color){0,0,0,80});
            GuiPanel(box, "Nouveau produit");

            float bx = box.x + 20, by = box.y + 40;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "SKU");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, f_sku, sizeof f_sku, edit_sku)) edit_sku = !edit_sku;

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Nom");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, f_name, sizeof f_name, edit_name)) edit_name = !edit_name;

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Categorie");
            if (GuiTextBox((Rectangle){ bx + 110, by, 280, 24 }, f_category, sizeof f_category, edit_cat)) edit_cat = !edit_cat;

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Prix unitaire");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, f_price, sizeof f_price, edit_price)) edit_price = !edit_price;

            by += 34;
            GuiLabel((Rectangle){ bx, by, 100, 24 }, "Seuil alerte");
            if (GuiTextBox((Rectangle){ bx + 110, by, 130, 24 }, f_threshold, sizeof f_threshold, edit_threshold)) edit_threshold = !edit_threshold;

            by += 50;
            if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Enregistrer")) {
                Product np = {0};
                snprintf(np.sku, sizeof np.sku, "%s", f_sku);
                snprintf(np.name, sizeof np.name, "%s", f_name);
                snprintf(np.category, sizeof np.category, "%s", f_category);
                snprintf(np.unit, sizeof np.unit, "pcs");
                np.unit_price = (float)atof(f_price);
                np.alert_threshold = atoi(f_threshold);

                char err[256];
                if (f_sku[0] == '\0' || f_name[0] == '\0') {
                    toast_show(&toast, "SKU et nom sont obligatoires", true);
                } else if (inv_add_product(&np, err, sizeof err)) {
                    toast_show(&toast, "Produit ajoute", false);
                    total_products = inv_all_products(&all_products);
                    panel = PANEL_NONE;
                } else {
                    toast_show(&toast, err, true);
                }
            }
            if (GuiButton((Rectangle){ bx + 150, by, 130, 32 }, "Annuler")) panel = PANEL_NONE;
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

            by += 40;
            GuiLabel((Rectangle){ bx, by, 90, 24 }, "Quantite");
            if (GuiTextBox((Rectangle){ bx + 100, by, 100, 24 }, m_qty, sizeof m_qty, edit_qty)) edit_qty = !edit_qty;

            by += 50;
            if (GuiButton((Rectangle){ bx, by, 130, 32 }, "Valider")) {
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
