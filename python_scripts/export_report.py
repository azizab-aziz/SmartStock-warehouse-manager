"""
export_report.py - reads a CSV written by the C app and produces a
styled .xlsx report. No pandas/numpy - just csv (stdlib) + openpyxl.

Usage (all args optional, fixed defaults used otherwise):
    python export_report.py [csv_path] [xlsx_path] [sheet_name]
"""

import csv
import os
import subprocess
import sys

DEFAULT_CSV = "exports/export.csv"
DEFAULT_XLSX = "exports/rapport_stock.xlsx"
DEFAULT_SHEET = "Rapport Stock"

HEADER_COLOR = "1E293B"   # navy - matches SmartStock's header bar
ROW_ALT_COLOR = "EFF6FF"  # very light blue - matches the app's row hover tint
GREEN = "10B981"          # matches COLOR_ACCENT_TEAL
RED = "DC2626"            # matches COLOR_ACCENT_RED

# Columns that must be written as real numbers (float), everything else
# stays text. Must match the header names written by inv_export_csv().
NUMERIC_COLUMNS = {"Quantite", "Prix_Unitaire", "Valeur_Stock", "Seuil_Alerte"}


def ensure_openpyxl():
    try:
        import openpyxl  # noqa: F401
    except ImportError:
        print("openpyxl introuvable, installation en cours...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "openpyxl"])


ensure_openpyxl()

from openpyxl import Workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter


def to_number(value):
    """Best-effort float conversion. Returns None if it isn't numeric,
    so the caller can fall back to writing the raw text instead."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def read_csv(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        return [], []
    return rows[0], rows[1:]


def build_workbook(headers, data, sheet_name):
    wb = Workbook()
    ws = wb.active
    ws.title = (sheet_name or DEFAULT_SHEET)[:31]  # Excel sheet-name limit

    thin = Side(style="thin", color="D0D5DD")
    border = Border(left=thin, right=thin, top=thin, bottom=thin)
    header_fill = PatternFill("solid", fgColor=HEADER_COLOR)
    header_font = Font(color="FFFFFF", bold=True)
    alt_fill = PatternFill("solid", fgColor=ROW_ALT_COLOR)

    # ---- header row ----
    for col_idx, header in enumerate(headers, start=1):
        cell = ws.cell(row=1, column=col_idx, value=header)
        cell.fill = header_fill
        cell.font = header_font
        cell.alignment = Alignment(horizontal="center", vertical="center")
        cell.border = border

    qty_idx = headers.index("Quantite") if "Quantite" in headers else None
    seuil_idx = headers.index("Seuil_Alerte") if "Seuil_Alerte" in headers else None
    statut_idx = headers.index("Statut") if "Statut" in headers else None

    # ---- data rows ----
    for row_offset, row in enumerate(data):
        r = row_offset + 2
        is_alt = row_offset % 2 == 1

        low_stock = False
        if qty_idx is not None and seuil_idx is not None:
            q = to_number(row[qty_idx]) if qty_idx < len(row) else None
            s = to_number(row[seuil_idx]) if seuil_idx < len(row) else None
            if q is not None and s is not None and q <= s:
                low_stock = True

        for col_idx, header in enumerate(headers, start=1):
            raw = row[col_idx - 1] if col_idx - 1 < len(row) else ""
            value = raw
            if header in NUMERIC_COLUMNS:
                num = to_number(raw)
                value = num if num is not None else raw  # fallback: keep as text, don't crash

            cell = ws.cell(row=r, column=col_idx, value=value)
            cell.border = border
            if is_alt:
                cell.fill = alt_fill

            # Business-meaning colors:
            #   Quantite   -> red+bold if at/under alert threshold, else default
            #   Valeur_Stock -> green (positive/asset value)
            #   Statut     -> red "STOCK FAIBLE" / green "OK"
            if header == "Quantite" and low_stock:
                cell.font = Font(color=RED, bold=True)
            elif header == "Valeur_Stock":
                cell.font = Font(color=GREEN)
            elif header == "Statut":
                cell.font = Font(color=RED if low_stock else GREEN, bold=True)

    # ---- column widths: longest content + margin ----
    for col_idx, header in enumerate(headers, start=1):
        max_len = len(header)
        for row in data:
            if col_idx - 1 < len(row):
                max_len = max(max_len, len(str(row[col_idx - 1])))
        ws.column_dimensions[get_column_letter(col_idx)].width = max_len + 4

    ws.freeze_panes = "A2"
    if data:
        ws.auto_filter.ref = f"A1:{get_column_letter(len(headers))}{len(data) + 1}"

    return wb


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    xlsx_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_XLSX
    sheet_name = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_SHEET

    if not os.path.isfile(csv_path):
        print(f"Erreur: fichier CSV introuvable: {csv_path}")
        sys.exit(1)

    headers, data = read_csv(csv_path)
    if not headers:
        print("Erreur: CSV vide")
        sys.exit(1)

    wb = build_workbook(headers, data, sheet_name)
    out_dir = os.path.dirname(xlsx_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    wb.save(xlsx_path)
    print("Excel exported successfully")


if __name__ == "__main__":
    main()