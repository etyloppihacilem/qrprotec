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

Equivalent de l'inventaire papier : pour chaque lot, les types d'items attendus tries par emplacement, avec des
cases a remplir (quantite trouvee, peremption la plus proche, OK). Un lot global donne une page par sous-lot (en
profondeur d'abord, comme partout ailleurs). Un lot trop long continue sur une page « suite ».

Le PDF est ecrit a la main (polices standard Helvetica, encodage WinAnsi) pour ne pas ajouter de dependance.
"""

import unicodedata
import zlib

from django.utils import timezone

from .models import LotRequirements, name_key

PAGE_WIDTH = 595.28   # A4 en points
PAGE_HEIGHT = 841.89
MARGIN = 40
CONTENT_WIDTH = PAGE_WIDTH - 2 * MARGIN

ROW_HEIGHT = 20
GROUP_HEIGHT = 18
TABLE_HEAD_HEIGHT = 22
SIGN_HEIGHT = 112      # bloc « verifie le / par / signature / remarques » en bas de la derniere page d'un lot
FOOTER_Y = PAGE_HEIGHT - 24

NO_LOCATION = 'Sans emplacement'
PAGE_REF = '\x01'  # entoure l'id d'un sous-lot dont le numero de page n'est connu qu'a la fin

# (titre, largeur) des colonnes du tableau
COLUMNS = [
    ("Type d'item", 255),
    ('Attendu', 50),
    ('Trouvé', 55),
    ('Péremption\nla plus proche', 105),
    ('OK', 50),
]

GREY_TEXT = 0.42
GREY_BAND = 0.90
GREY_HEAD = 0.80
GREY_DISABLED = 0.94


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
        self.first_page = {}   # id du lot -> numero de sa premiere page
        self.page = None
        self.y = 0

    # -- mise en page -------------------------------------------------------------------------------------------------

    def new_page(self, lot, continued=False):
        self.page = Page()
        self.pages.append((self.page, lot))
        self.first_page.setdefault(lot.id, len(self.pages))
        self.y = MARGIN
        self.header(lot, continued)

    def header(self, lot, continued):
        page = self.page
        page.text(MARGIN, self.y + 8, "FICHE D'INVENTAIRE", size=8, bold=True, grey=GREY_TEXT)
        page.text(PAGE_WIDTH - MARGIN, self.y + 8, 'Éditée le ' + self.edited.strftime('%d/%m/%Y'), size=8,
                  grey=GREY_TEXT, align='right')
        self.y += 30
        title = lot.name + (' (suite)' if continued else '')
        page.text(MARGIN, self.y, fit(title, CONTENT_WIDTH, 20, True), size=20, bold=True)
        self.y += 18
        details = f'{lot.lot_type.name} · {lot.id} · version {lot.version}'
        page.text(MARGIN, self.y, fit(details, CONTENT_WIDTH, 10), size=10, grey=GREY_TEXT)
        self.y += 14
        if continued:
            self.y += 6
            return
        ancestors = lot.ancestors()
        if ancestors:
            path = ' › '.join(parent.name for parent in reversed(ancestors))
            page.text(MARGIN, self.y, fit('Dans : ' + path, CONTENT_WIDTH, 10), size=10)
            self.y += 14
        children = [sub for sub in self.lots if sub.parent_id == lot.id]
        if children:
            names = ', '.join(sub.name for sub in children)
            page.text(MARGIN, self.y, fit('Sous-lots : ' + names, CONTENT_WIDTH, 10), size=10)
            self.y += 14
        for line in wrap(lot.lot_type.description, CONTENT_WIDTH, 9, max_lines=3):
            page.text(MARGIN, self.y, line, size=9, grey=GREY_TEXT)
            self.y += 12
        self.y += 8

    def space_left(self, reserve=0):
        return FOOTER_Y - 16 - reserve - self.y

    def table_head(self):
        page = self.page
        x = MARGIN
        for label, width in COLUMNS:
            page.rect(x, self.y, width, TABLE_HEAD_HEIGHT, fill=GREY_HEAD)
            lines = label.split('\n')
            baseline = self.y + 14 - (len(lines) - 1) * 4.5
            for line in lines:
                page.text(x + width / 2 if x > MARGIN else x + 6, baseline, fit(line, width - 6, 8, True), size=8,
                          bold=True, align='center' if x > MARGIN else 'left')
                baseline += 9
            x += width
        self.y += TABLE_HEAD_HEIGHT

    def group_row(self, location, continued=False):
        self.page.rect(MARGIN, self.y, CONTENT_WIDTH, GROUP_HEIGHT, fill=GREY_BAND)
        label = location + (' (suite)' if continued else '')
        self.page.text(MARGIN + 6, self.y + 12.5, fit(label, CONTENT_WIDTH - 12, 9.5, True), size=9.5, bold=True)
        self.y += GROUP_HEIGHT

    def item_row(self, requirement):
        page = self.page
        item_type = requirement.item_type
        x = MARGIN
        widths = [width for _, width in COLUMNS]
        for index, width in enumerate(widths):
            perishable_cell = index == 3 and not item_type.perissable
            page.rect(x, self.y, width, ROW_HEIGHT, fill=GREY_DISABLED if perishable_cell else None)
            x += width
        baseline = self.y + 13.5
        name = item_type.name
        note = 'étiquette à déchirer' if item_type.tear_off else ''
        name_width = widths[0] - 12
        if note:
            note_width = text_width(note, 7.5) + 6
            shown = fit(name, name_width - note_width, 10)
            page.text(MARGIN + 6, baseline, shown, size=10)
            page.text(MARGIN + 6 + text_width(shown, 10) + 6, baseline, note, size=7.5, grey=GREY_TEXT)
        else:
            page.text(MARGIN + 6, baseline, fit(name, name_width, 10), size=10)
        x = MARGIN + widths[0]
        page.text(x + widths[1] / 2, baseline, str(requirement.quantity), size=11, bold=True, align='center')
        x += widths[1] + widths[2]
        if not item_type.perissable:
            page.text(x + widths[3] / 2, baseline, 'non périssable', size=7.5, grey=GREY_TEXT, align='center')
        else:
            page.text(x + widths[3] / 2, baseline, '....../....../............', size=8, grey=0.6, align='center')
        x += widths[3]
        box = 11
        page.rect(x + (widths[4] - box) / 2, self.y + (ROW_HEIGHT - box) / 2, box, box, line_width=0.8)
        self.y += ROW_HEIGHT

    def signature(self):
        page = self.page
        self.y = max(self.y + 14, FOOTER_Y - 16 - SIGN_HEIGHT)
        top = self.y
        third = CONTENT_WIDTH / 3
        for index, label in enumerate(('Vérifié le', 'Par', 'Signature')):
            x = MARGIN + index * third
            page.text(x, top + 10, label, size=9, bold=True)
            page.line(x, top + 30, x + third - 14, top + 30, grey=0.5)
        page.text(MARGIN, top + 48, 'Remarques (manquants, périmés, réassort à prévoir)', size=9, bold=True)
        page.rect(MARGIN, top + 54, CONTENT_WIDTH, SIGN_HEIGHT - 58, line_width=0.5)

    # -- contenu ------------------------------------------------------------------------------------------------------

    def lot_pages(self, lot):
        self.new_page(lot)
        groups = requirement_groups(lot)
        if not groups:
            self.page.text(MARGIN, self.y + 10, 'Aucun item attendu dans ce lot.', size=11, bold=True)
            self.y += 26
            children = [sub for sub in self.lots if sub.parent_id == lot.id]
            if children:
                self.page.text(MARGIN, self.y, 'Il regroupe les sous-lots suivants, chacun sur sa page :', size=10)
                self.y += 18
                self.children_list(lot)
            return
        total = sum(len(rows) for _, rows in groups)
        self.table_head()
        done = 0
        for location, rows in groups:
            # un emplacement ne commence pas en bas de page sans au moins un item
            if self.space_left(GROUP_HEIGHT + ROW_HEIGHT) < 0:
                self.new_page(lot, continued=True)
                self.table_head()
            self.group_row(location)
            for requirement in rows:
                last = done == total - 1
                # la derniere ligne doit laisser la place du bloc de signature
                if self.space_left(ROW_HEIGHT + (SIGN_HEIGHT + 14 if last else 0)) < 0:
                    self.new_page(lot, continued=True)
                    self.table_head()
                    self.group_row(location, continued=True)
                self.item_row(requirement)
                done += 1
        self.signature()

    def children_list(self, lot):
        for sub in self.lots:
            if sub is lot or sub.depth <= lot.depth:
                continue
            if not any(parent.id == lot.id for parent in sub.ancestors()):
                continue
            if self.space_left(SIGN_HEIGHT + 30) < 0:
                break
            indent = MARGIN + 12 + (sub.depth - lot.depth - 1) * 14
            self.page.text(indent, self.y, fit(f'• {sub.name} ({sub.lot_type.name})', CONTENT_WIDTH - indent, 10),
                           size=10)
            self.page.text(PAGE_WIDTH - MARGIN - 45, self.y, f'page {PAGE_REF}{sub.id}{PAGE_REF}', size=10,
                           grey=GREY_TEXT)
            self.y += 15

    def write(self):
        for lot in self.lots:
            self.lot_pages(lot)
        total = len(self.pages)
        for number, (page, lot) in enumerate(self.pages, start=1):
            # renvois « page {id} » de la liste des sous-lots, connus une fois toutes les pages placees
            page.ops = [self._resolve(op) for op in page.ops]
            page.line(MARGIN, FOOTER_Y - 10, PAGE_WIDTH - MARGIN, FOOTER_Y - 10, line_width=0.4, grey=0.6)
            where = f'{self.root.name} › {lot.name}' if lot is not self.root else lot.name
            page.text(MARGIN, FOOTER_Y, fit(f'QRProtec · {where} ({lot.id})', CONTENT_WIDTH - 80, 8), size=8,
                      grey=GREY_TEXT)
            page.text(PAGE_WIDTH - MARGIN, FOOTER_Y, f'page {number}/{total}', size=8, grey=GREY_TEXT, align='right')
        return build_pdf([page for page, _ in self.pages], title=f"Fiche d'inventaire - {self.root.name}")

    def _resolve(self, op):
        marker = PAGE_REF.encode()
        if marker not in op:
            return op
        start = op.index(marker)
        end = op.index(marker, start + 1)
        lot_id = op[start + 1:end].decode()
        return op[:start] + str(self.first_page.get(lot_id, '?')).encode() + op[end + 1:]


def lot_sheet_pdf(lot):
    """PDF de la fiche du lot et de ses sous-lots actifs (une page par lot, plus si besoin)."""
    return SheetWriter(lot).write()
