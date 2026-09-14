"""
export_report.py - reads two CSVs written by the C app (products, movement
history) and fills them into a macro-enabled template (report_template.xlsm)
that already contains a Workbook_Open VBA macro building a real
PivotTable + PivotChart + Slicer on its "Dashboard" sheet, sourced from a
hidden "Data_Raw" Excel Table this script (re)writes on every export.
No pandas/numpy - just csv (stdlib) + openpyxl.

Usage:
    python export_report.py [csv_path] [xlsx_path] [sheet_name] [movements_csv]
"""

import csv
import os
import subprocess
import sys

DEFAULT_CSV = "exports/export.csv"
DEFAULT_XLSX = "exports/rapport_stock.xlsm"
DEFAULT_SHEET = "Produits"
DEFAULT_MOV_CSV = "exports/export_movements.csv"
TEMPLATE_PATH = "python_scripts/templates/report_template.xlsm"

HEADER_COLOR = "1E293B"
SUBHEADER_COLOR = "334155"
ROW_ALT_COLOR = "EFF6FF"
SUMMARY_COLOR = "0F172A"
GREEN = "10B981"
RED = "DC2626"

CSV_COLUMNS = ["PRD", "SKU", "Nom", "Categorie", "Unite",
               "Quantite", "Prix_Unitaire", "Valeur_Stock", "Seuil_Alerte", "Statut"]
DISPLAY_COLUMNS = ["PRD", "SKU", "Nom", "Unite", "Quantite", "Prix_Unitaire", "Valeur_Stock", "Seuil_Alerte", "Statut"]
NUMERIC_COLUMNS = {"Quantite", "Prix_Unitaire", "Valeur_Stock", "Seuil_Alerte"}

MOV_COLUMNS = ["Date", "Produit", "Qte", "Type", "Reference", "Utilisateur", "Raison"]


def ensure_openpyxl():
    try:
        import openpyxl  # noqa: F401
    except ImportError:
        print("openpyxl introuvable, installation en cours...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "openpyxl"])


ensure_openpyxl()

from openpyxl import load_workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter
from openpyxl.worksheet.table import Table, TableStyleInfo


def to_number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def read_csv(path):
    if not path or not os.path.isfile(path):
        return [], []
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        return [], []
    return rows[0], rows[1:]


def group_by_category(headers, data):
    cat_idx = headers.index("Categorie")
    groups = []
    current_cat, current_rows = None, []
    for row in data:
        cat = row[cat_idx] if cat_idx < len(row) else ""
        if cat != current_cat and current_rows:
            groups.append((current_cat, current_rows))
            current_rows = []
        current_cat = cat
        current_rows.append(row)
    if current_rows:
        groups.append((current_cat, current_rows))
    return groups


def clear_worksheet(ws):
    """Wipes a sheet's content while keeping the same sheet object (and
    therefore the same VBA codename Excel tracks internally) - critical
    so the macro's Sheets("Data_Raw")/("Dashboard") lookups keep working
    across every re-export, instead of deleting+recreating sheets."""
    for merged in list(ws.merged_cells.ranges):
        ws.unmerge_cells(str(merged))
    if ws.max_row > 0:
        ws.delete_rows(1, ws.max_row)
    for name in list(ws.tables.keys()):
        del ws.tables[name]


def build_products_sheet(ws, headers, data, sheet_name):
    thin = Side(style="thin", color="D0D5DD")
    border = Border(left=thin, right=thin, top=thin, bottom=thin)
    alt_fill = PatternFill("solid", fgColor=ROW_ALT_COLOR)
    col_count = len(DISPLAY_COLUMNS)

    qty_idx = CSV_COLUMNS.index("Quantite")
    value_idx = CSV_COLUMNS.index("Valeur_Stock")
    threshold_idx = CSV_COLUMNS.index("Seuil_Alerte")

    total_products = len(data)
    total_qty = sum((to_number(r[qty_idx]) or 0) for r in data if qty_idx < len(r))
    total_value = sum((to_number(r[value_idx]) or 0) for r in data if value_idx < len(r))
    groups = group_by_category(headers, data)

    ws.merge_cells(start_row=1, start_column=1, end_row=1, end_column=col_count)
    title_cell = ws.cell(row=1, column=1, value="Resume general")
    title_cell.fill = PatternFill("solid", fgColor=SUMMARY_COLOR)
    title_cell.font = Font(color="FFFFFF", bold=True, size=13)
    title_cell.alignment = Alignment(horizontal="left", vertical="center")
    ws.row_dimensions[1].height = 24

    summary_labels = ["Categories", "Produits", "Unites en stock", "Valeur totale du stock"]
    summary_values = [len(groups), total_products, int(total_qty), f"{total_value:.2f} DT"]
    for ci, (label, value) in enumerate(zip(summary_labels, summary_values), start=1):
        lc = ws.cell(row=2, column=ci, value=label)
        lc.font = Font(bold=True, color=HEADER_COLOR)
        lc.border = border
        vc = ws.cell(row=3, column=ci, value=value)
        vc.font = Font(size=12, bold=True, color=GREEN)
        vc.border = border
    row_cursor = 5

    for cat_name, rows in groups:
        display_name = cat_name if cat_name else "Sans categorie"

        ws.merge_cells(start_row=row_cursor, start_column=1, end_row=row_cursor, end_column=col_count)
        band = ws.cell(row=row_cursor, column=1, value=display_name)
        band.fill = PatternFill("solid", fgColor=HEADER_COLOR)
        band.font = Font(color="FFFFFF", bold=True, size=12)
        band.alignment = Alignment(horizontal="left", vertical="center")
        ws.row_dimensions[row_cursor].height = 22
        row_cursor += 1

        for ci, h in enumerate(DISPLAY_COLUMNS, start=1):
            c = ws.cell(row=row_cursor, column=ci, value=h)
            c.fill = PatternFill("solid", fgColor=SUBHEADER_COLOR)
            c.font = Font(color="FFFFFF", bold=True)
            c.alignment = Alignment(horizontal="center", vertical="center")
            c.border = border
        row_cursor += 1

        cat_qty_total = 0.0
        cat_value_total = 0.0

        for i, row in enumerate(rows):
            is_alt = i % 2 == 1
            low_stock = False
            q = to_number(row[qty_idx]) if qty_idx < len(row) else None
            s = to_number(row[threshold_idx]) if threshold_idx < len(row) else None
            if q is not None and s is not None and q <= s:
                low_stock = True
            if q is not None:
                cat_qty_total += q
            v = to_number(row[value_idx]) if value_idx < len(row) else None
            if v is not None:
                cat_value_total += v

            for ci, h in enumerate(DISPLAY_COLUMNS, start=1):
                src_idx = CSV_COLUMNS.index(h)
                raw = row[src_idx] if src_idx < len(row) else ""
                value = raw
                if h in NUMERIC_COLUMNS:
                    num = to_number(raw)
                    value = num if num is not None else raw

                cell = ws.cell(row=row_cursor, column=ci, value=value)
                cell.border = border
                if is_alt:
                    cell.fill = alt_fill

                if h == "Quantite" and low_stock:
                    cell.font = Font(color=RED, bold=True)
                elif h == "Valeur_Stock":
                    cell.font = Font(color=GREEN)
                elif h == "Statut":
                    cell.font = Font(color=RED if low_stock else GREEN, bold=True)

            row_cursor += 1

        sub_label = ws.cell(row=row_cursor, column=1, value=f"Sous-total ({len(rows)} produit(s))")
        sub_label.font = Font(bold=True, color=HEADER_COLOR)
        qty_col = DISPLAY_COLUMNS.index("Quantite") + 1
        value_col = DISPLAY_COLUMNS.index("Valeur_Stock") + 1
        qc = ws.cell(row=row_cursor, column=qty_col, value=int(cat_qty_total))
        qc.font = Font(bold=True)
        vc = ws.cell(row=row_cursor, column=value_col, value=round(cat_value_total, 2))
        vc.font = Font(bold=True, color=GREEN)
        for ci in range(1, col_count + 1):
            ws.cell(row=row_cursor, column=ci).border = border
        row_cursor += 2

    for ci, h in enumerate(DISPLAY_COLUMNS, start=1):
        max_len = len(h)
        src_idx = CSV_COLUMNS.index(h)
        for row in data:
            if src_idx < len(row):
                max_len = max(max_len, len(str(row[src_idx])))
        ws.column_dimensions[get_column_letter(ci)].width = max_len + 4


def build_history_sheet(ws, mov_headers, mov_data):
    thin = Side(style="thin", color="D0D5DD")
    border = Border(left=thin, right=thin, top=thin, bottom=thin)
    alt_fill = PatternFill("solid", fgColor=ROW_ALT_COLOR)
    col_count = len(MOV_COLUMNS)

    ws.merge_cells(start_row=1, start_column=1, end_row=1, end_column=col_count)
    title_cell = ws.cell(row=1, column=1, value=f"Historique des mouvements ({len(mov_data)} mouvement(s))")
    title_cell.fill = PatternFill("solid", fgColor=SUMMARY_COLOR)
    title_cell.font = Font(color="FFFFFF", bold=True, size=13)
    title_cell.alignment = Alignment(horizontal="left", vertical="center")
    ws.row_dimensions[1].height = 24

    for ci, h in enumerate(MOV_COLUMNS, start=1):
        c = ws.cell(row=3, column=ci, value=h)
        c.fill = PatternFill("solid", fgColor=HEADER_COLOR)
        c.font = Font(color="FFFFFF", bold=True)
        c.alignment = Alignment(horizontal="center", vertical="center")
        c.border = border

    qty_idx = MOV_COLUMNS.index("Qte")
    if not mov_data:
        return

    for i, row in enumerate(mov_data):
        r = i + 4
        is_alt = i % 2 == 1
        for ci, h in enumerate(MOV_COLUMNS, start=1):
            src_idx = MOV_COLUMNS.index(h)
            raw = row[src_idx] if src_idx < len(row) else ""
            cell = ws.cell(row=r, column=ci, value=raw)
            cell.border = border
            if is_alt:
                cell.fill = alt_fill
            if ci - 1 == qty_idx:
                is_pos = raw.strip().startswith("+")
                cell.font = Font(color=GREEN if is_pos else RED, bold=True)

    for ci, h in enumerate(MOV_COLUMNS, start=1):
        max_len = len(h)
        src_idx = MOV_COLUMNS.index(h)
        for row in mov_data:
            if src_idx < len(row):
                max_len = max(max_len, len(str(row[src_idx])))
        ws.column_dimensions[get_column_letter(ci)].width = max_len + 4

    ws.freeze_panes = "A4"
    ws.auto_filter.ref = f"A3:{get_column_letter(col_count)}{len(mov_data) + 3}"


def build_data_raw_sheet(ws, data):
    """Flat, unstyled table - PRD/SKU/Nom/Categorie/Unite/Quantite/
    Prix_Unitaire/Valeur_Stock/Seuil_Alerte/Statut, one row per product,
    no merges or subtotals. This is the PivotTable's real source data;
    the pretty grouped "Produits" sheet can't be used directly since
    PivotTables need a clean rectangular range."""
    headers = CSV_COLUMNS
    for ci, h in enumerate(headers, start=1):
        ws.cell(row=1, column=ci, value=h)

    row_count = len(data)
    for ri, row in enumerate(data, start=2):
        for ci, h in enumerate(headers, start=1):
            raw = row[ci - 1] if ci - 1 < len(row) else ""
            value = raw
            if h in NUMERIC_COLUMNS:
                num = to_number(raw)
                value = num if num is not None else raw
            ws.cell(row=ri, column=ci, value=value)

    last_row = row_count + 1
    if row_count == 0:
        # a Table needs at least one data row - write a harmless blank one
        for ci in range(1, len(headers) + 1):
            ws.cell(row=2, column=ci, value="")
        last_row = 2

    ref = f"A1:{get_column_letter(len(headers))}{last_row}"
    tbl = Table(displayName="ProduitsTable", ref=ref)
    tbl.tableStyleInfo = TableStyleInfo(name="TableStyleMedium9", showRowStripes=True)
    ws.add_table(tbl)


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    xlsx_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_XLSX
    sheet_name = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_SHEET
    mov_csv_path = sys.argv[4] if len(sys.argv) > 4 else DEFAULT_MOV_CSV

    if not os.path.isfile(csv_path):
        print(f"Erreur: fichier CSV introuvable: {csv_path}")
        sys.exit(1)
    if not os.path.isfile(TEMPLATE_PATH):
        print(f"Erreur: modele macro introuvable: {TEMPLATE_PATH} "
              f"(voir les instructions de configuration du dashboard)")
        sys.exit(1)

    headers, data = read_csv(csv_path)
    if not headers:
        print("Erreur: CSV vide")
        sys.exit(1)

    mov_headers, mov_data = read_csv(mov_csv_path)

    wb = load_workbook(TEMPLATE_PATH, keep_vba=True)

    if "Produits" in wb.sheetnames:
        ws_prod = wb["Produits"]
        clear_worksheet(ws_prod)
    else:
        ws_prod = wb.create_sheet("Produits")
    build_products_sheet(ws_prod, headers, data, sheet_name)

    if "Historique" in wb.sheetnames:
        ws_hist = wb["Historique"]
        clear_worksheet(ws_hist)
    else:
        ws_hist = wb.create_sheet("Historique")
    build_history_sheet(ws_hist, mov_headers, mov_data)

    if "Data_Raw" in wb.sheetnames:
        ws_raw = wb["Data_Raw"]
        clear_worksheet(ws_raw)
    else:
        ws_raw = wb.create_sheet("Data_Raw")
    build_data_raw_sheet(ws_raw, data)
    ws_raw.sheet_state = "hidden"  # the Workbook_Open macro re-hides it too; belt and suspenders

    if "Dashboard" not in wb.sheetnames:
        wb.create_sheet("Dashboard")
    # Dashboard's actual PivotTable/PivotChart/Slicer are intentionally
    # NOT touched here - they're rebuilt automatically by the template's
    # Workbook_Open macro every time the file is opened in Excel, using
    # whatever fresh data just got written to Data_Raw above.

    if not xlsx_path.lower().endswith(".xlsm"):
        xlsx_path = os.path.splitext(xlsx_path)[0] + ".xlsm"

    out_dir = os.path.dirname(xlsx_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    wb.save(xlsx_path)
    print("Excel exported successfully")


if __name__ == "__main__":
    main()