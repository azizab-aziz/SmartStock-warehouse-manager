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
NAVY_DARK = "#0F172A"
SUBHEADER = "#334155"
TEAL = "#10B981"
RED = "#DC2626"
MUTED = "#64748B"
BORDER = "#E2E8F0"
ALT_ROW = "#EFF6FF"


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

PAGE_W, PAGE_H = A4
BAR_H = 20 * mm
FOOTER_H = 12 * mm


def read_csv(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        return [], []
    return rows[0], rows[1:]


def draw_page_frame(canvas, doc, product_label):
    """Header + footer bars, drawn on every page - gives the sheet the
    same navy header-bar identity as the SmartStock desktop app."""
    canvas.saveState()

    # top bar
    canvas.setFillColor(colors.HexColor(NAVY_DARK))
    canvas.rect(0, PAGE_H - BAR_H, PAGE_W, BAR_H, fill=1, stroke=0)
    canvas.setFillColor(colors.white)
    canvas.setFont("Helvetica-Bold", 13)
    canvas.drawString(18 * mm, PAGE_H - BAR_H + 6.5 * mm, "SmartStock")
    canvas.setFont("Helvetica", 9)
    canvas.setFillColor(colors.HexColor("#94A3B8"))
    canvas.drawRightString(PAGE_W - 18 * mm, PAGE_H - BAR_H + 7 * mm, "FICHE PRODUIT")

    # footer bar
    canvas.setStrokeColor(colors.HexColor(BORDER))
    canvas.setLineWidth(0.75)
    canvas.line(18 * mm, FOOTER_H, PAGE_W - 18 * mm, FOOTER_H)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(colors.HexColor(MUTED))
    canvas.drawString(18 * mm, FOOTER_H - 5 * mm,
                       f"Genere le {datetime.now().strftime('%d/%m/%Y a %H:%M')}")
    canvas.drawRightString(PAGE_W - 18 * mm, FOOTER_H - 5 * mm, product_label)
    canvas.drawCentredString(PAGE_W / 2, FOOTER_H - 5 * mm, f"Page {doc.page}")

    canvas.restoreState()


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
        topMargin=BAR_H + 12 * mm, bottomMargin=FOOTER_H + 6 * mm,
        leftMargin=18 * mm, rightMargin=18 * mm,
    )
    elements = []

    title_style = ParagraphStyle("title", fontName="Helvetica-Bold", fontSize=20,
                                  leading=24, textColor=colors.HexColor(NAVY), spaceAfter=4)
    sub_style = ParagraphStyle("sub", fontName="Helvetica", fontSize=11, leading=14,
                                textColor=colors.HexColor(MUTED), spaceBefore=2, spaceAfter=16)
    section_style = ParagraphStyle("section", fontName="Helvetica-Bold", fontSize=12, leading=16,
                                    textColor=colors.white, spaceBefore=0, spaceAfter=0)
    label_style = ParagraphStyle("lbl", fontName="Helvetica-Bold", fontSize=9.5,
                                  leading=13, textColor=colors.white)
    value_style = ParagraphStyle("val", fontName="Helvetica", fontSize=10.5,
                                  leading=14, textColor=colors.HexColor(NAVY))

    product_name = info.get("Nom", "")
    elements.append(Paragraph(f"Fiche Produit — {product_name}", title_style))
    elements.append(Paragraph(
        f"{info.get('PRD', '')} &nbsp;·&nbsp; SKU {info.get('SKU', '')}", sub_style))

    # ---- info table ----
    statut = info.get("Statut", "")
    statut_color = RED if statut == "STOCK FAIBLE" else TEAL
    statut_style = ParagraphStyle("statut", fontName="Helvetica-Bold", fontSize=10.5,
                                   leading=14, textColor=colors.HexColor(statut_color))

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

    info_table = Table(info_data, colWidths=[52 * mm, 113 * mm])
    info_style_cmds = [
        ("BACKGROUND", (0, 0), (0, -1), colors.HexColor(NAVY)),
        ("BACKGROUND", (1, 0), (1, -1), colors.white),
        ("BOX", (0, 0), (-1, -1), 1, colors.HexColor(NAVY)),
        ("INNERGRID", (0, 0), (-1, -1), 0.5, colors.HexColor(BORDER)),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 10),
        ("RIGHTPADDING", (0, 0), (-1, -1), 10),
        ("TOPPADDING", (0, 0), (-1, -1), 7),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
    ]
    for r in range(1, len(info_data)):
        if r % 2 == 0:
            info_style_cmds.append(("BACKGROUND", (1, r), (1, r), colors.HexColor(ALT_ROW)))
    info_table.setStyle(TableStyle(info_style_cmds))
    elements.append(info_table)
    elements.append(Spacer(1, 22))

    # ---- movement history section band ----
    section_table = Table([[Paragraph("Historique des mouvements", section_style)]],
                           colWidths=[165 * mm], rowHeights=[9 * mm])
    section_table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), colors.HexColor(NAVY)),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 10),
    ]))
    elements.append(section_table)

    if not mov_rows:
        elements.append(Spacer(1, 10))
        elements.append(Paragraph("Aucun mouvement enregistre pour ce produit.", value_style))
    else:
        header_style = ParagraphStyle("mh", fontName="Helvetica-Bold", fontSize=8.5,
                                       leading=11, textColor=colors.white)
        cell_style = ParagraphStyle("mc", fontName="Helvetica", fontSize=8.5,
                                     leading=11, textColor=colors.HexColor(NAVY))

        table_data = [[Paragraph(h, header_style) for h in mov_headers]]
        qty_idx = mov_headers.index("Qte") if "Qte" in mov_headers else None
        for row in mov_rows[:25]:  # keep a one-page sheet readable
            cells = []
            for ci, val in enumerate(row):
                if ci == qty_idx:
                    is_pos = val.strip().startswith("+")
                    st = ParagraphStyle("qv", parent=cell_style,
                                         fontName="Helvetica-Bold",
                                         textColor=colors.HexColor(TEAL if is_pos else RED))
                    cells.append(Paragraph(val, st))
                else:
                    cells.append(Paragraph(val, cell_style))
            table_data.append(cells)

        # Date / Qte / Type / Reference / Utilisateur / Raison
        col_widths = [26 * mm, 13 * mm, 22 * mm, 28 * mm, 24 * mm, 52 * mm][:len(mov_headers)]
        mov_table = Table(table_data, colWidths=col_widths, repeatRows=1)
        style_cmds = [
            ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor(SUBHEADER)),
            ("BOX", (0, 0), (-1, -1), 1, colors.HexColor(NAVY)),
            ("INNERGRID", (0, 0), (-1, -1), 0.4, colors.HexColor(BORDER)),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
            ("LEFTPADDING", (0, 0), (-1, -1), 6),
            ("RIGHTPADDING", (0, 0), (-1, -1), 6),
            ("TOPPADDING", (0, 0), (-1, -1), 5),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ]
        for r in range(1, len(table_data)):
            if r % 2 == 0:
                style_cmds.append(("BACKGROUND", (0, r), (-1, r), colors.HexColor(ALT_ROW)))
        mov_table.setStyle(TableStyle(style_cmds))
        elements.append(mov_table)

        if len(mov_rows) > 25:
            elements.append(Spacer(1, 6))
            footnote_style = ParagraphStyle("fn", fontName="Helvetica-Oblique", fontSize=8.5,
                                             textColor=colors.HexColor(MUTED))
            elements.append(Paragraph(
                f"+ {len(mov_rows) - 25} mouvement(s) plus ancien(s) non affiches", footnote_style))

    product_label = f"{info.get('PRD', '')} — {product_name}"
    doc.build(elements,
              onFirstPage=lambda c, d: draw_page_frame(c, d, product_label),
              onLaterPages=lambda c, d: draw_page_frame(c, d, product_label))
    print("PDF exported successfully")


if __name__ == "__main__":
    main()