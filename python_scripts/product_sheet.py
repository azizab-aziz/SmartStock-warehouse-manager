"""
product_sheet.py - generates a one-page PDF "fiche produit" from two CSVs
written by the C app: product info (1 row) and movement history (N rows).
Uses reportlab (auto-installed if missing) - no other dependencies.

Usage:
    python product_sheet.py <info_csv> <movements_csv> <output_pdf>
"""

import csv
import os
import subprocess
import sys
from datetime import datetime

NAVY = "#1E293B"
TEAL = "#10B981"
RED = "#DC2626"
MUTED = "#64748B"
BORDER = "#E2E8F0"


def ensure_reportlab():
    try:
        import reportlab  # noqa: F401
    except ImportError:
        print("reportlab introuvable, installation en cours...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "reportlab"])


ensure_reportlab()

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle


def read_csv(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        return [], []
    return rows[0], rows[1:]


def main():
    if len(sys.argv) < 4:
        print("Usage: product_sheet.py <info_csv> <movements_csv> <output_pdf>")
        sys.exit(1)

    info_csv, mov_csv, out_pdf = sys.argv[1], sys.argv[2], sys.argv[3]

    if not os.path.isfile(info_csv):
        print(f"Erreur: fichier introuvable: {info_csv}")
        sys.exit(1)

    info_headers, info_rows = read_csv(info_csv)
    if not info_rows:
        print("Erreur: aucune donnee produit")
        sys.exit(1)
    info = dict(zip(info_headers, info_rows[0]))

    mov_headers, mov_rows = [], []
    if os.path.isfile(mov_csv):
        mov_headers, mov_rows = read_csv(mov_csv)

    out_dir = os.path.dirname(out_pdf)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    doc = SimpleDocTemplate(
        out_pdf, pagesize=A4,
        topMargin=20 * mm, bottomMargin=18 * mm,
        leftMargin=18 * mm, rightMargin=18 * mm,
    )
    elements = []

    title_style = ParagraphStyle("title", fontName="Helvetica-Bold", fontSize=20,
                                  textColor=colors.HexColor(NAVY), spaceAfter=2)
    sub_style = ParagraphStyle("sub", fontName="Helvetica", fontSize=11,
                                textColor=colors.HexColor(MUTED), spaceAfter=14)
    section_style = ParagraphStyle("section", fontName="Helvetica-Bold", fontSize=12,
                                    textColor=colors.HexColor(NAVY), spaceBefore=14, spaceAfter=6)
    footer_style = ParagraphStyle("footer", fontName="Helvetica", fontSize=8,
                                   textColor=colors.HexColor(MUTED))

    elements.append(Paragraph(f"Fiche Produit — {info.get('Nom', '')}", title_style))
    elements.append(Paragraph(f"{info.get('PRD', '')}  ·  SKU {info.get('SKU', '')}", sub_style))

    # ---- info table ----
    statut = info.get("Statut", "")
    statut_color = RED if statut == "STOCK FAIBLE" else TEAL

    label_style = ParagraphStyle("lbl", fontName="Helvetica-Bold", fontSize=9, textColor=colors.white)
    value_style = ParagraphStyle("val", fontName="Helvetica", fontSize=10, textColor=colors.HexColor(NAVY))
    statut_style = ParagraphStyle("statut", fontName="Helvetica-Bold", fontSize=10,
                                   textColor=colors.HexColor(statut_color))

    fields = [
        ("Categorie", info.get("Categorie", "")),
        ("Unite", info.get("Unite", "")),
        ("Quantite en stock", info.get("Quantite", "")),
        ("Prix unitaire", f"{info.get('Prix_Unitaire', '')} DT"),
        ("Valeur du stock", f"{info.get('Valeur_Stock', '')} DT"),
        ("Seuil d'alerte", info.get("Seuil_Alerte", "")),
    ]

    info_data = [[Paragraph(label, label_style), Paragraph(str(value), value_style)]
                 for label, value in fields]
    info_data.append([Paragraph("Statut", label_style), Paragraph(statut, statut_style)])

    info_table = Table(info_data, colWidths=[55 * mm, 110 * mm])
    info_table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (0, -1), colors.HexColor(NAVY)),
        ("BACKGROUND", (1, 0), (1, -1), colors.white),
        ("GRID", (0, 0), (-1, -1), 0.5, colors.HexColor(BORDER)),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 6),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
    ]))
    elements.append(info_table)

    # ---- movement history ----
    elements.append(Paragraph("Historique des mouvements", section_style))

    if not mov_rows:
        elements.append(Paragraph("Aucun mouvement enregistre pour ce produit.", value_style))
    else:
        header_style = ParagraphStyle("mh", fontName="Helvetica-Bold", fontSize=8.5, textColor=colors.white)
        cell_style = ParagraphStyle("mc", fontName="Helvetica", fontSize=8.5, textColor=colors.HexColor(NAVY))

        table_data = [[Paragraph(h, header_style) for h in mov_headers]]
        qty_idx = mov_headers.index("Qte") if "Qte" in mov_headers else None
        for row in mov_rows[:25]:  # keep a one-page sheet readable
            cells = []
            for ci, val in enumerate(row):
                if ci == qty_idx:
                    is_pos = val.strip().startswith("+")
                    st = ParagraphStyle("qv", parent=cell_style,
                                         textColor=colors.HexColor(TEAL if is_pos else RED))
                    cells.append(Paragraph(val, st))
                else:
                    cells.append(Paragraph(val, cell_style))
            table_data.append(cells)

        col_widths = [28 * mm, 15 * mm, 22 * mm, 25 * mm, 25 * mm, 50 * mm][:len(mov_headers)]
        mov_table = Table(table_data, colWidths=col_widths, repeatRows=1)
        style_cmds = [
            ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor(NAVY)),
            ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor(BORDER)),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
            ("LEFTPADDING", (0, 0), (-1, -1), 5),
            ("TOPPADDING", (0, 0), (-1, -1), 4),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ]
        for r in range(1, len(table_data)):
            if r % 2 == 0:
                style_cmds.append(("BACKGROUND", (0, r), (-1, r), colors.HexColor("#EFF6FF")))
        mov_table.setStyle(TableStyle(style_cmds))
        elements.append(mov_table)

        if len(mov_rows) > 25:
            elements.append(Spacer(1, 4))
            elements.append(Paragraph(
                f"+ {len(mov_rows) - 25} mouvement(s) plus ancien(s) non affiches", footer_style))

    elements.append(Spacer(1, 20))
    elements.append(Paragraph(
        f"Genere le {datetime.now().strftime('%d/%m/%Y a %H:%M')} — SmartStock WMS", footer_style))

    doc.build(elements)
    print("PDF exported successfully")


if __name__ == "__main__":
    main()