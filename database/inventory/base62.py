# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          base62.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Sep. 2026 at 18:37
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

import string

ALPHABET = string.digits + string.ascii_uppercase + string.ascii_lowercase  # 62 caractères : 0-9, A-Z, a-z
BASE = len(ALPHABET)
CHAR_TO_INDEX = {char: i for i, char in enumerate(ALPHABET)}


def encode_base62(number: int, length: int = 6) -> str:
    """Encode un entier positif en base62, sur `length` caractères, complété par des zéros à gauche."""
    max_value = BASE ** length - 1
    if not (0 <= number <= max_value):
        raise ValueError(f"number doit être entre 0 et {max_value} pour tenir sur {length} caractères")

    chars = []
    n = number
    for _ in range(length):
        n, remainder = divmod(n, BASE)
        chars.append(ALPHABET[remainder])

    return ''.join(reversed(chars))


def decode_base62(encoded: str) -> int:
    """Décode une chaîne base62 en entier."""
    number = 0
    for char in encoded:
        number = number * BASE + CHAR_TO_INDEX[char]
    return number
