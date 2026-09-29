# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          models.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Sep. 2026 at 18:37
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

import hmac
import secrets
import string
from datetime import timedelta

from django.conf import settings
from django.db import models
from django.db import transaction
from django.utils import timezone

from .base62 import encode_base62

# iid = TYPE (6) + YYYYMMDD (8) + compteur base62 (8)
TYPE_LENGTH = 6
DATE_LENGTH = 8
COUNTER_LENGTH = 8
IID_LENGTH = TYPE_LENGTH + DATE_LENGTH + COUNTER_LENGTH
NO_DATE = '00000000'  # date utilisee dans l'iid pour les items non perissables

KEY_ALPHABET = string.digits + string.ascii_letters
KEY_LENGTH = 24


def generate_key(length: int = KEY_LENGTH) -> str:
    return ''.join(secrets.choice(KEY_ALPHABET) for _ in range(length))


def keys_match(expected: str | None, given: str | None) -> bool:
    if not expected or not given:
        return False
    return hmac.compare_digest(expected.encode(), given.encode())


def qrprotec_setting(name):
    return settings.QRPROTEC[name]


def peremption_code(peremption) -> str:
    return peremption.strftime('%Y%m%d') if peremption else NO_DATE


class SeenWhile(models.TextChoices):
    CREATE = 'create', 'Creation'
    VERIF = 'verif', 'Verification'
    ADD = 'add', 'Ajout a un lot'
    REMOVE = 'remove', 'Retour en stock'
    DELETE = 'delete', 'Suppression manuelle'
    RESTORE = 'restore', 'Restauration'
    OPEN = 'open', 'Ouverture de paquet'


class ItemStatus(models.TextChoices):
    ACTIVE = 'active', 'Present'
    MISSING = 'missing', 'Disparu'
    REPLACED = 'replaced', 'Remplace'
    DELETED = 'deleted', 'Supprime manuellement'


# models.py
class ItemsPacksManager(models.Manager):
    def add_items(self, item_type, peremption, count, user, location=None, context=SeenWhile.CREATE, sealed_pack=None):
        if count <= 0:
            raise ValueError("La quantite doit etre positive")
        if item_type.perissable and peremption is None:
            raise ValueError(f"Le type {item_type.type} est perissable : une date de peremption est obligatoire")
        if not item_type.perissable:
            peremption = None
        now = timezone.now()
        pack_id = f"{item_type.type}{peremption_code(peremption)}"

        with transaction.atomic():
            pack, _ = self.select_for_update().get_or_create(
                id=pack_id,
                defaults={'item_type': item_type, 'peremption': peremption, 'last_sequence': 0}
            )
            start = pack.last_sequence

            new_items = [
                Items(
                    iid=f"{pack_id}{encode_base62(start + i + 1, length=COUNTER_LENGTH)}",
                    pack=pack,
                    location=location,
                    sealed_pack=sealed_pack,
                    last_seen=now,
                    last_seen_by=user,
                    last_seen_while=context,
                    added=now,
                    added_by=user,
                )
                for i in range(count)
            ]
            Items.objects.bulk_create(new_items)
            pack.last_sequence = start + count
            pack.save(update_fields=['last_sequence'])

        return new_items


# Create your models here.


class Sequence(models.Model):
    """Compteur generique (paquets scelles, ...)."""
    name = models.CharField(max_length=32, primary_key=True)
    last_sequence = models.PositiveBigIntegerField(default=0)

    @classmethod
    def next(cls, name: str) -> int:
        with transaction.atomic():
            seq, _ = cls.objects.select_for_update().get_or_create(name=name)
            seq.last_sequence += 1
            seq.save(update_fields=['last_sequence'])
            return seq.last_sequence


class ItemType(models.Model):
    type = models.CharField(max_length=TYPE_LENGTH, primary_key=True, editable=False)
    name = models.CharField(max_length=64)
    description = models.TextField(blank=True, default='')
    min_quantity = models.PositiveIntegerField(default=0) # minimum value that should be in stock
    perissable = models.BooleanField(default=False) # if false, date is not mandatory
    default_pack_size = models.PositiveIntegerField(default=1) # nombre d'items dans un paquet a la reception

    def __str__(self):
        return f"{self.name} ({self.type})"


class ItemsPacks(models.Model):
    """Regroupe les items d'un meme type et d'une meme date : sert de prefixe a l'iid."""
    id = models.CharField(max_length=TYPE_LENGTH + DATE_LENGTH, primary_key=True, editable=False)
    item_type = models.ForeignKey(ItemType, on_delete=models.PROTECT, db_column="type", to_field="type")
    peremption = models.DateField(blank=True, null=True)
    last_sequence = models.PositiveIntegerField(default=0)

    objects = ItemsPacksManager()

    def save(self, *args, **kwargs):
        if not self.id:
            self.id = self._build_id()
        super().save(*args, **kwargs)

    def _build_id(self):
        return f"{self.item_type_id}{peremption_code(self.peremption)}"


class SealedPacks(models.Model):
    """Paquet ferme (ex: boite de compresses). Les items existent deja en base, leurs etiquettes
    individuelles sont imprimees a l'ouverture du paquet."""
    id = models.CharField(max_length=COUNTER_LENGTH, primary_key=True, editable=False)
    item_type = models.ForeignKey(ItemType, on_delete=models.PROTECT)
    peremption = models.DateField(blank=True, null=True)
    count = models.PositiveIntegerField()
    created = models.DateTimeField(default=timezone.now)
    created_by = models.CharField(max_length=32)
    opened = models.DateTimeField(blank=True, null=True)
    opened_by = models.CharField(max_length=32, blank=True, default='')

    def save(self, *args, **kwargs):
        if not self.id:
            self.id = encode_base62(Sequence.next('sealed_pack'), length=COUNTER_LENGTH)
        super().save(*args, **kwargs)


class Items(models.Model):
    iid = models.CharField(max_length=IID_LENGTH, primary_key=True, editable=False)
    pack = models.ForeignKey(ItemsPacks, on_delete=models.PROTECT, related_name="items")
    location = models.ForeignKey(
        'Lots',
        on_delete=models.SET_NULL,
        null=True,
        blank=True,
        related_name='items',
    ) # None = en stock
    sealed_pack = models.ForeignKey(
        SealedPacks, on_delete=models.SET_NULL, null=True, blank=True, related_name='items'
    )
    status = models.CharField(max_length=8, choices=ItemStatus.choices, default=ItemStatus.ACTIVE)
    missed_verifs = models.PositiveIntegerField(default=0) # nombre de verifs consecutives sans le voir
    last_seen = models.DateTimeField()
    last_seen_by = models.CharField(max_length=64) # should be user id but could be something else
    last_seen_while = models.CharField(max_length=8, choices=SeenWhile.choices)
    added = models.DateTimeField()
    added_by = models.CharField(max_length=32) # should be user as well (and admin)
    deleted = models.DateTimeField(blank=True, null=True)
    deleted_by = models.CharField(max_length=32, blank=True, default='')
    deleted_reason = models.CharField(max_length=128, blank=True, default='')

    @property
    def item_type(self):
        return self.pack.item_type

    @property
    def peremption(self):
        return self.pack.peremption

    def is_expired(self, today=None) -> bool:
        peremption = self.pack.peremption
        if peremption is None:
            return False
        return peremption < (today or timezone.localdate())

    def touch(self, user, context, now=None):
        self.last_seen = now or timezone.now()
        self.last_seen_by = user
        self.last_seen_while = context


class LotType(models.Model):
    type = models.CharField(max_length=TYPE_LENGTH, primary_key=True)
    name = models.CharField(max_length=64)
    description = models.TextField(blank=True, default='')
    created = models.DateTimeField(default=timezone.now)
    created_by = models.CharField(max_length=32, blank=True, default='')
    version = models.PositiveIntegerField(default=1)
    valid_version = models.PositiveIntegerField(default=1)

    def __str__(self):
        return f"{self.name} ({self.type})"


class LotSequence(models.Model):
    lot_type = models.OneToOneField(LotType, on_delete=models.PROTECT, primary_key=True)
    last_sequence = models.PositiveIntegerField(default=0)


class LotRequirements(models.Model): # Pour mettre un item dans un lot
    lot_type = models.ForeignKey(LotType, on_delete=models.CASCADE, related_name="requirements")
    item_type = models.ForeignKey(ItemType, on_delete=models.PROTECT)
    quantity = models.PositiveIntegerField(default=1)

    class Meta:
        unique_together = [('lot_type', 'item_type')]


def default_lot_key_expiration():
    return timezone.localdate() + timedelta(days=qrprotec_setting('LOT_KEY_VALIDITY_DAYS'))


class Lots(models.Model):
    id = models.CharField(max_length=16, primary_key=True, editable=False)
    lot_type = models.ForeignKey(LotType, on_delete=models.PROTECT, db_column='type', to_field='type')
    version = models.PositiveIntegerField()
    verif_key = models.CharField(max_length=32, default=generate_key)
    verif_key_expires = models.DateField(default=default_lot_key_expiration)
    created = models.DateTimeField(default=timezone.now)
    created_by = models.CharField(max_length=32)
    last_used = models.DateTimeField(blank=True, null=True)
    last_used_by = models.CharField(max_length=64, blank=True, default='')
    last_verif = models.DateTimeField(blank=True, null=True)
    last_verif_by = models.CharField(max_length=64, blank=True, default='')
    last_verif_while = models.CharField(max_length=8, blank=True, default='')
    is_sealed = models.BooleanField(default=False)
    active = models.BooleanField(default=True)
    name = models.CharField(max_length=64)
    name_short = models.CharField(max_length=16)

    def save(self, *args, **kwargs):
        if not self.version:
            self.version = self.lot_type.version
        if not self.id:
            self.id = self._build_id()
        super().save(*args, **kwargs)

    def _build_id(self):
        with transaction.atomic():
            seq, _ = LotSequence.objects.select_for_update().get_or_create(
                lot_type=self.lot_type
            )
            seq.last_sequence += 1
            seq.save(update_fields=['last_sequence'])
            unique_part = encode_base62(seq.last_sequence, length=8)
        return f"{self.lot_type_id}{unique_part}"

    def check_key(self, key) -> bool:
        return keys_match(self.verif_key, key) and self.verif_key_expires >= timezone.localdate()

    def rotate_key(self):
        self.verif_key = generate_key()
        self.verif_key_expires = default_lot_key_expiration()

    def __str__(self):
        return f"{self.name} ({self.id})"


class Secouristes(models.Model):
    matricule = models.CharField(max_length=16, primary_key=True, editable=False)
    nom = models.CharField(max_length=32)
    prenom = models.CharField(max_length=32)
    key = models.CharField(max_length=32, blank=True, null=True)
    key_expires = models.DateField(blank=True, null=True)
    privileged = models.BooleanField(default=False) # acces au mode privilegie du logiciel
    active = models.BooleanField(default=True)
    created = models.DateTimeField(default=timezone.now)

    def renew_key(self):
        self.key = generate_key()
        self.key_expires = timezone.localdate() + timedelta(days=qrprotec_setting('USER_KEY_VALIDITY_DAYS'))

    def check_key(self, key) -> bool:
        return (
            self.active
            and self.key_expires is not None
            and self.key_expires >= timezone.localdate()
            and keys_match(self.key, key)
        )

    def __str__(self):
        return f"{self.prenom} {self.nom}"


class VerifResult(models.TextChoices):
    PRESENT = 'present', 'Present'
    MISSING = 'missing', 'Absent'
    REPLACED = 'replaced', 'Remplace'


class Verifs(models.Model):
    lot = models.ForeignKey(Lots, on_delete=models.CASCADE, null=True, blank=True) # None = verif du stock
    datetime = models.DateTimeField(default=timezone.now)
    by = models.CharField(max_length=64)
    complete = models.BooleanField(default=False) # toutes les exigences du type de lot sont remplies
    present_count = models.PositiveIntegerField(default=0)
    missing_count = models.PositiveIntegerField(default=0)
    expired_count = models.PositiveIntegerField(default=0)
    replaced_count = models.PositiveIntegerField(default=0)


class VerifItem(models.Model):
    verif = models.ForeignKey(Verifs, on_delete=models.CASCADE, related_name='entries')
    item = models.ForeignKey(Items, on_delete=models.CASCADE)
    result = models.CharField(max_length=8, choices=VerifResult.choices, default=VerifResult.PRESENT)
    expired = models.BooleanField(default=False)
