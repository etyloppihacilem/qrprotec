# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          qr.py
#       -\-    _|__
#        |\___/  . \        Created on 15 Oct. 2026 at 14:15
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""QR code minimal (mode octet, correction M, versions 1 a 10), pour dessiner l'etiquette publique d'un lot dans un
PDF sans ajouter de dependance. Ecrit d'apres la norme ISO/IEC 18004.
"""

# (octets de correction par bloc, octets de donnees de chaque bloc), correction M
_BLOCKS = {
    1: (10, [16]),
    2: (16, [28]),
    3: (26, [44]),
    4: (18, [32] * 2),
    5: (24, [43] * 2),
    6: (16, [27] * 4),
    7: (18, [31] * 4),
    8: (22, [38] * 2 + [39] * 2),
    9: (22, [36] * 3 + [37] * 2),
    10: (26, [43] * 4 + [44]),
}
_ALIGNMENT = {1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30], 6: [6, 34], 7: [6, 22, 38], 8: [6, 24, 42],
              9: [6, 26, 46], 10: [6, 28, 50]}

_EXP = [0] * 512
_LOG = [0] * 256
_value = 1
for _power in range(255):
    _EXP[_power] = _value
    _LOG[_value] = _power
    _value <<= 1
    if _value & 0x100:
        _value ^= 0x11d
for _power in range(255, 512):
    _EXP[_power] = _EXP[_power - 255]


def _multiply(a, b):
    return 0 if a == 0 or b == 0 else _EXP[_LOG[a] + _LOG[b]]


def _ecc(data, degree):
    generator = [1]
    for power in range(degree):
        generator = [a ^ _multiply(b, _EXP[power]) for a, b in zip(generator + [0], [0] + generator)]
    remainder = [0] * degree
    for byte in data:
        factor = byte ^ remainder[0]
        remainder = remainder[1:] + [0]
        for index in range(degree):
            remainder[index] ^= _multiply(generator[index + 1], factor)
    return remainder


def _codewords(payload):
    for version, (ecc_length, blocks) in _BLOCKS.items():
        capacity = sum(blocks)
        count_bits = 8 if version < 10 else 16
        if 4 + count_bits + 8 * len(payload) <= 8 * capacity:
            break
    else:
        raise ValueError('Texte trop long pour le QR code')
    bits = [0, 1, 0, 0] + [(len(payload) >> shift) & 1 for shift in range(count_bits - 1, -1, -1)]
    for byte in payload:
        bits += [(byte >> shift) & 1 for shift in range(7, -1, -1)]
    bits += [0] * min(4, 8 * capacity - len(bits))
    bits += [0] * (-len(bits) % 8)
    data = [int(''.join(map(str, bits[index:index + 8])), 2) for index in range(0, len(bits), 8)]
    data += [0xec, 0x11] * ((capacity - len(data)) // 2) + [0xec] * ((capacity - len(data)) % 2)
    split, start = [], 0
    for length in blocks:
        split.append(data[start:start + length])
        start += length
    eccs = [_ecc(block, ecc_length) for block in split]
    result = [block[index] for index in range(max(blocks)) for block in split if index < len(block)]
    result += [ecc[index] for index in range(ecc_length) for ecc in eccs]
    return version, result


class _Matrix:
    def __init__(self, version):
        self.size = 17 + 4 * version
        self.dark = [[False] * self.size for _ in range(self.size)]
        self.function = [[False] * self.size for _ in range(self.size)]
        self.version = version

    def set(self, x, y, dark):
        self.dark[y][x] = dark
        self.function[y][x] = True

    def draw_functions(self):
        size = self.size
        for index in range(size):
            self.set(6, index, index % 2 == 0)
            self.set(index, 6, index % 2 == 0)
        for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    if 0 <= cx + dx < size and 0 <= cy + dy < size:
                        self.set(cx + dx, cy + dy, max(abs(dx), abs(dy)) not in (2, 4))
        positions = _ALIGNMENT[self.version]
        last = len(positions) - 1
        for i, x in enumerate(positions):
            for j, y in enumerate(positions):
                if (i, j) in ((0, 0), (0, last), (last, 0)):
                    continue
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        self.set(x + dx, y + dy, max(abs(dx), abs(dy)) != 1)
        self.draw_format(0)
        if self.version >= 7:
            remainder = self.version
            for _ in range(12):
                remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1f25)
            bits = self.version << 12 | remainder
            for index in range(18):
                dark = (bits >> index) & 1 == 1
                a, b = size - 11 + index % 3, index // 3
                self.set(a, b, dark)
                self.set(b, a, dark)

    def draw_format(self, mask):
        data = 0 << 3 | mask  # correction M : 00
        remainder = data
        for _ in range(10):
            remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537)
        bits = (data << 10 | remainder) ^ 0x5412
        bit = [(bits >> index) & 1 == 1 for index in range(15)]
        size = self.size
        for index in range(6):
            self.set(8, index, bit[index])
        self.set(8, 7, bit[6])
        self.set(8, 8, bit[7])
        self.set(7, 8, bit[8])
        for index in range(9, 15):
            self.set(14 - index, 8, bit[index])
        for index in range(8):
            self.set(size - 1 - index, 8, bit[index])
        for index in range(8, 15):
            self.set(8, size - 15 + index, bit[index])
        self.set(8, size - 8, True)

    def draw_data(self, codewords):
        size, index, total = self.size, 0, len(codewords) * 8
        right = size - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vertical in range(size):
                for offset in (0, 1):
                    x = right - offset
                    y = size - 1 - vertical if (right + 1) & 2 == 0 else vertical
                    if not self.function[y][x] and index < total:
                        self.dark[y][x] = (codewords[index >> 3] >> (7 - (index & 7))) & 1 == 1
                        index += 1
            right -= 2

    def apply_mask(self, mask):
        test = [
            lambda x, y: (x + y) % 2 == 0,
            lambda x, y: y % 2 == 0,
            lambda x, y: x % 3 == 0,
            lambda x, y: (x + y) % 3 == 0,
            lambda x, y: (x // 3 + y // 2) % 2 == 0,
            lambda x, y: x * y % 2 + x * y % 3 == 0,
            lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
            lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0,
        ][mask]
        for y in range(self.size):
            for x in range(self.size):
                if not self.function[y][x] and test(x, y):
                    self.dark[y][x] = not self.dark[y][x]

    def penalty(self):
        size, score = self.size, 0
        lines = [row for row in self.dark] + [[self.dark[y][x] for y in range(size)] for x in range(size)]
        finder_like = ('10111010000', '00001011101')
        for line in lines:
            run, previous = 0, None
            for module in line:
                run = run + 1 if module == previous else 1
                previous = module
                if run == 5:
                    score += 3
                elif run > 5:
                    score += 1
            text = '0000' + ''.join('1' if module else '0' for module in line) + '0000'
            score += 40 * sum(text.count(pattern) for pattern in finder_like)
        for y in range(size - 1):
            for x in range(size - 1):
                if self.dark[y][x] == self.dark[y][x + 1] == self.dark[y + 1][x] == self.dark[y + 1][x + 1]:
                    score += 3
        dark = sum(map(sum, self.dark))
        score += 10 * (abs(dark * 20 - size * size * 10) // (size * size))
        return score


def qr_matrix(text, mask=None):
    """Modules du QR code de text : liste de lignes de booleens (True = noir), sans marge."""
    version, codewords = _codewords(str(text).encode('utf-8'))
    best = None
    for candidate in range(8) if mask is None else [mask]:
        matrix = _Matrix(version)
        matrix.draw_functions()
        matrix.draw_data(codewords)
        matrix.apply_mask(candidate)
        matrix.draw_format(candidate)
        score = matrix.penalty()
        if best is None or score < best[0]:
            best = (score, matrix)
    return best[1].dark
