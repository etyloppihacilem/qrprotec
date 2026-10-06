# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          lot_sheet.py
#       -\-    _|__
#        |\___/  . \        Created on 05 Oct. 2026 at 08:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Fiche d'inventaire papier d'un lot, en PDF A4.

Liste de controle pour verifier sur le terrain qu'un sac est complet, sans reseau ni poste : les types d'items
attendus, regroupes par emplacement, sur deux colonnes avec une case a cocher. La taille du texte diminue pour que la
liste tienne en une page autant que possible ; au-dela, elle continue sur une page « suite ». Un lot global donne une
page par sous-lot (en profondeur d'abord, comme partout ailleurs) ; un lot de regroupement sans item attendu n'a pas
de page a lui.

Le PDF est ecrit a la main (polices standard Helvetica, encodage WinAnsi) pour ne pas ajouter de dependance.
"""

import unicodedata
import zlib

from django.utils import timezone

from .models import LotRequirements, name_key

PAGE_WIDTH = 595.28   # A4 en points
PAGE_HEIGHT = 841.89
MARGIN = 36
CONTENT_WIDTH = PAGE_WIDTH - 2 * MARGIN
FOOTER_Y = PAGE_HEIGHT - 22
BODY_BOTTOM = FOOTER_Y - 16

COLUMN_GAP = 18
COLUMN_WIDTH = (CONTENT_WIDTH - COLUMN_GAP) / 2

NO_LOCATION = 'Sans emplacement'

# tailles essayees dans l'ordre, jusqu'a ce que la liste tienne en une page : (texte, interligne, bandeau)
SCALES = [(11, 19, 20), (10, 17, 18), (9, 15, 16), (8, 13, 14)]

GREY_TEXT = 0.42
GREY_BAND = 0.88


# ----------------------------------------------------------------------------------------------------------------------
# Largeur du texte (metriques AFM des polices standard, caracteres 32 a 126)

_HELVETICA = [
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833,
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556,
    556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334,
    260, 334, 584,
]
_HELVETICA_BOLD = [
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833,
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611,
    556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389,
    280, 389, 584,
]
_SPECIAL_WIDTHS = {'«': 556, '»': 556, '’': 222, '‘': 222, '–': 556, '—': 1000, '…': 1000, '°': 400, '€': 556,
                   'œ': 944, 'Œ': 1000, '·': 278, '›': 333, ' ': 278}


def _char_width(character, bold):
    table = _HELVETICA_BOLD if bold else _HELVETICA
    code = ord(character)
    if 32 <= code <= 126:
        return table[code - 32]
    if character in _SPECIAL_WIDTHS:
        return _SPECIAL_WIDTHS[character]
    base = unicodedata.normalize('NFKD', character)[:1]  # lettre accentuee : largeur de la lettre de base
    if base and 32 <= ord(base) <= 126:
        return table[ord(base) - 32]
    return 556


def text_width(text, size, bold=False):
    return sum(_char_width(character, bold) for character in text) * size / 1000


def fit(text, width, size, bold=False):
    """Coupe le texte avec « … » pour qu'il tienne dans la largeur."""
    text = ' '.join(str(text).split())
    if text_width(text, size, bold) <= width:
        return text
    while text and text_width(text + '…', size, bold) > width:
        text = text[:-1]
    return text.rstrip() + '…'


def wrap(text, width, size, bold=False, max_lines=3):
    lines = []
    for paragraph in str(text).splitlines():
        line = ''
        for word in paragraph.split():
            candidate = f'{line} {word}' if line else word
            if text_width(candidate, size, bold) <= width or not line:
                line = candidate
            else:
                lines.append(line)
                line = word
        if line:
            lines.append(line)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = fit(lines[-1] + ' …', width, size, bold)
    return [fit(line, width, size, bold) for line in lines]


# ----------------------------------------------------------------------------------------------------------------------
# Ecriture du PDF

def _pdf_string(text):
    encoded = bytearray()
    for character in str(text):
        try:
            encoded += character.encode('cp1252')
        except UnicodeEncodeError:
            base = unicodedata.normalize('NFKD', character).encode('cp1252', 'ignore')
            encoded += base or b'?'
    return b'(' + bytes(encoded).replace(b'\\', b'\\\\').replace(b'(', b'\\(').replace(b')', b'\\)') + b')'


def _num(value):
    return f'{value:.2f}'.rstrip('0').rstrip('.')


class Page:
    """Operations de dessin d'une page, en coordonnees « depuis le haut » (y croit vers le bas)."""

    def __init__(self):
        self.ops = []

    def text(self, x, y, text, size=10, bold=False, grey=0.0, align='left'):
        if align == 'right':
            x -= text_width(text, size, bold)
        elif align == 'center':
            x -= text_width(text, size, bold) / 2
        font = b'/F2' if bold else b'/F1'
        self.ops.append(b'%s g BT %s %s Tf %s %s Td %s Tj ET' % (
            _num(grey).encode(), font, _num(size).encode(), _num(x).encode(), _num(PAGE_HEIGHT - y).encode(),
            _pdf_string(text)))

    def rect(self, x, y, width, height, fill=None, stroke=True, line_width=0.6):
        """Rectangle de coin superieur gauche (x, y). fill : niveau de gris, ou None."""
        ops = [b'%s w' % _num(line_width).encode()]
        if fill is not None:
            ops.append(b'%s g' % _num(fill).encode())
        ops.append(b'%s %s %s %s re' % (_num(x).encode(), _num(PAGE_HEIGHT - y - height).encode(),
                                        _num(width).encode(), _num(height).encode()))
        ops.append(b'B' if fill is not None and stroke else b'f' if fill is not None else b'S')
        self.ops.append(b'0 G ' + b' '.join(ops))

    def line(self, x1, y1, x2, y2, line_width=0.6, grey=0.0):
        self.ops.append(b'%s G %s w %s %s m %s %s l S' % (
            _num(grey).encode(), _num(line_width).encode(), _num(x1).encode(), _num(PAGE_HEIGHT - y1).encode(),
            _num(x2).encode(), _num(PAGE_HEIGHT - y2).encode()))

    def stream(self):
        return zlib.compress(b'\n'.join(self.ops))


def build_pdf(pages, title=''):
    objects = []  # contenu des objets 1..n

    def add(content):
        objects.append(content)
        return len(objects)

    catalog = add(None)
    pages_id = add(None)
    font = add(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    font_bold = add(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>')
    kids = []
    for page in pages:
        data = page.stream()
        content = add(b'<< /Length %d /Filter /FlateDecode >>\nstream\n%s\nendstream' % (len(data), data))
        kids.append(add(b'<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] /Contents %d 0 R '
                        b'/Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> >> >>' % (
                            pages_id, _num(PAGE_WIDTH).encode(), _num(PAGE_HEIGHT).encode(), content, font,
                            font_bold)))
    objects[catalog - 1] = b'<< /Type /Catalog /Pages %d 0 R >>' % pages_id
    objects[pages_id - 1] = b'<< /Type /Pages /Kids [%s] /Count %d >>' % (
        b' '.join(b'%d 0 R' % kid for kid in kids), len(kids))
    info = add(b'<< /Title %s /Producer (QRProtec) >>' % _pdf_string(title))

    out = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = []
    for number, content in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b'%d 0 obj\n%s\nendobj\n' % (number, content)
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1)
    for offset in offsets:
        out += b'%010d 00000 n \n' % offset
    out += b'trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (
        len(objects) + 1, catalog, info, xref)
    return bytes(out)


# ----------------------------------------------------------------------------------------------------------------------
# Contenu de la fiche

def requirement_groups(lot):
    """[(emplacement, [exigences])] tries par emplacement (sans accents ni casse), « sans emplacement » en dernier,
    et les types d'items par nom dans chaque emplacement."""
    groups = {}
    labels = {}
    requirements = LotRequirements.objects.select_related('item_type').filter(lot_type_id=lot.lot_type_id)
    for requirement in requirements:
        location = ' '.join(requirement.location.split())
        key = name_key(location)
        labels.setdefault(key, location)
        groups.setdefault(key, []).append(requirement)
    ordered = sorted(groups, key=lambda key: (key == '', key))
    return [
        (labels[key] or NO_LOCATION, sorted(groups[key], key=lambda row: (name_key(row.item_type.name), row.item_type_id)))
        for key in ordered
    ]


class SheetWriter:
    def __init__(self, root):
        self.root = root
        self.lots = root.descendants()
        self.edited = timezone.localtime()
        self.pages = []        # [(page, lot)]
        self._groups = {}

    def groups(self, lot):
        if lot.id not in self._groups:
            self._groups[lot.id] = requirement_groups(lot)
        return self._groups[lot.id]

    # -- en-tete ------------------------------------------------------------------------------------------------------

    def header(self, lot, continued, page=None):
        """Dessine l'en-tete (si page) et renvoie la hauteur occupee depuis le haut de la page."""
        def text(*args, **kwargs):
            if page is not None:
                page.text(*args, **kwargs)

        y = MARGIN
        text(MARGIN, y + 8, "FICHE D'INVENTAIRE", size=8, bold=True, grey=GREY_TEXT)
        text(PAGE_WIDTH - MARGIN, y + 8, 'Éditée le ' + self.edited.strftime('%d/%m/%Y'), size=8, grey=GREY_TEXT,
             align='right')
        y += 28
        count = sum(requirement.quantity for _, rows in self.groups(lot) for requirement in rows)
        title = lot.name + (' (suite)' if continued else '')
        text(MARGIN, y, fit(title, CONTENT_WIDTH, 18, True), size=18, bold=True)
        y += 15
        ancestors = lot.ancestors()
        details = [lot.lot_type.name, lot.id, f'{count} item' + ('s' if count > 1 else '')]
        if ancestors:
            details.insert(0, 'dans ' + ' › '.join(parent.name for parent in reversed(ancestors)))
        text(MARGIN, y, fit(' · '.join(details), CONTENT_WIDTH, 9.5), size=9.5, grey=GREY_TEXT)
        y += 13
        children = [sub for sub in self.lots if sub.parent_id == lot.id]
        if children and not continued:
            names = ', '.join(sub.name for sub in children)
            text(MARGIN, y, fit('Sous-lots (une page chacun) : ' + names, CONTENT_WIDTH, 9.5), size=9.5)
            y += 13
        y += 4
        if page is not None:
            page.line(MARGIN, y, PAGE_WIDTH - MARGIN, y, line_width=0.8)
        return y + 10

    # -- liste ----------------------------------------------------------------------------------------------------

    @staticmethod
    def entries(groups, scale):
        """[(type, contenu, hauteur)] : bandeau d'emplacement puis ses items, le nom sur deux lignes au plus."""
        size, line_height, band = scale
        result = []
        for location, rows in groups:
            result.append(('location', location, band + 3))
            for requirement in rows:
                lines = wrap(requirement.item_type.name, name_width(size), size, max_lines=2)
                result.append(('item', (requirement, lines), line_height + (len(lines) - 1) * (size + 1)))
        return result

    def layout(self, lot, entries, scale, bottom=BODY_BOTTOM):
        """Place les entrees colonne par colonne : [[(colonne, y, entree)] par page]. Un emplacement ne commence pas
        en bas de colonne sans son premier item ; coupe, il est repete en tete de la colonne suivante (« suite »)."""
        band = scale[2] + 3
        pages = []
        top = self.header(lot, False)
        column, y = 0, top
        location = None
        pages.append([])

        def next_column():
            nonlocal column, y, top
            column += 1
            if column == 2:
                top = self.header(lot, True)
                pages.append([])
                column = 0
            y = top

        for index, entry in enumerate(entries):
            kind, content, height = entry
            if kind == 'location':
                location = content
                following = entries[index + 1][2] if index + 1 < len(entries) else 0
                if y + height + following > bottom and y > top:
                    next_column()
            else:
                if y + height > bottom and y > top:
                    next_column()
                    if location is not None:
                        pages[-1].append((column, y, ('location', location + ' (suite)', band)))
                        y += band
            pages[-1].append((column, y, entry))
            y += height
        return pages

    def lot_pages(self, lot):
        groups = self.groups(lot)
        for scale in SCALES:
            entries = self.entries(groups, scale)
            layouts = self.layout(lot, entries, scale)
            if len(layouts) == 1:
                break
        if len(layouts) == 1:
            # colonnes equilibrees : la plus petite hauteur de colonne qui garde la liste sur une page
            low, high = self.header(lot, False), BODY_BOTTOM
            while high - low > 1:
                middle = (low + high) / 2
                if len(self.layout(lot, entries, scale, middle)) == 1:
                    high = middle
                else:
                    low = middle
            layouts = self.layout(lot, entries, scale, high)
        for number, placed in enumerate(layouts):
            page = Page()
            self.pages.append((page, lot))
            self.header(lot, number > 0, page)
            for column, y, entry in placed:
                self.draw(page, MARGIN + column * (COLUMN_WIDTH + COLUMN_GAP), y, entry, scale)
            if not placed:
                page.text(MARGIN, self.header(lot, False) + 12, 'Aucun item attendu dans ce lot.', size=11, bold=True)
            elif any(column == 1 for column, _, _ in placed):  # filet entre les deux colonnes
                x = MARGIN + COLUMN_WIDTH + COLUMN_GAP / 2
                bottom = max(y + height for _, y, (_, _, height) in placed)
                page.line(x, self.header(lot, number > 0), x, bottom, line_width=0.4, grey=0.7)

    @staticmethod
    def draw(page, x, y, entry, scale):
        size, line_height, _ = scale
        kind, content, height = entry
        if kind == 'location':
            page.rect(x, y, COLUMN_WIDTH, height - 3, fill=GREY_BAND, stroke=False)
            page.text(x + 5, y + (height - 3) / 2 + size * 0.36, fit(content, COLUMN_WIDTH - 10, size, True),
                      size=size, bold=True)
            return
        requirement, lines = content
        baseline = y + (line_height + size * 0.72) / 2
        box = size * 0.95
        page.rect(x + 3, baseline - box * 0.85, box, box, line_width=0.8)
        quantity_right = x + 3 + box + 6 + text_width('000', size, True)
        page.text(quantity_right, baseline, f'{requirement.quantity}', size=size, bold=True, align='right')
        page.text(quantity_right + 2, baseline, '×', size=size * 0.8, grey=GREY_TEXT)
        name_x = x + COLUMN_WIDTH - name_width(size)
        for line in lines:
            page.text(name_x, baseline, line, size=size)
            baseline += size + 1
        page.line(name_x, y + height, x + COLUMN_WIDTH, y + height, line_width=0.3, grey=0.8)

    def write(self):
        for lot in self.lots:
            # un lot de regroupement (sans item attendu) n'a pas de page, sauf s'il est seul
            if self.groups(lot) or self.lots == [lot]:
                self.lot_pages(lot)
        if not self.pages:  # aucun lot n'attend d'item
            self.lot_pages(self.root)
        total = len(self.pages)
        for number, (page, lot) in enumerate(self.pages, start=1):
            page.line(MARGIN, FOOTER_Y - 10, PAGE_WIDTH - MARGIN, FOOTER_Y - 10, line_width=0.4, grey=0.6)
            where = f'{self.root.name} › {lot.name}' if lot is not self.root else lot.name
            page.text(MARGIN, FOOTER_Y, fit(f'QRProtec · {where} ({lot.id}) · version {lot.version}',
                                            CONTENT_WIDTH - 80, 8), size=8, grey=GREY_TEXT)
            page.text(PAGE_WIDTH - MARGIN, FOOTER_Y, f'page {number}/{total}', size=8, grey=GREY_TEXT, align='right')
        return build_pdf([page for page, _ in self.pages], title=f"Fiche d'inventaire - {self.root.name}")


def name_width(size):
    """Largeur du nom d'un item : la colonne moins la case et la quantite."""
    return COLUMN_WIDTH - (3 + size * 0.95 + 6 + text_width('000', size, True) + text_width(' ×', size) + 6)


def lot_sheet_pdf(lot):
    """PDF de la fiche du lot et de ses sous-lots actifs (une page par lot qui contient des items, plus si besoin)."""
    return SheetWriter(lot).write()
