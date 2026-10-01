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
import re
import secrets
import string
from datetime import timedelta

from django.conf import settings
from django.contrib.auth.hashers import check_password, make_password
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
    low_notified = models.BooleanField(default=False) # SMS "stock bas" deja envoye (remis a zero au-dessus du minimum)
    empty_notified = models.BooleanField(default=False) # notification "stock vide" deja envoyee (remis a zero au-dessus de 0)

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
    location = models.CharField(max_length=64, blank=True, default='') # ex: "pochette bleue"

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
    # Scelle : le lot ne peut pas etre ouvert sans briser le scelle, il n'a donc pas besoin de verif.
    # seal_code change a chaque scellage : l'etiquette d'un ancien scelle n'est plus valide.
    is_sealed = models.BooleanField(default=False)
    sealed = models.DateTimeField(blank=True, null=True)
    sealed_by = models.CharField(max_length=64, blank=True, default='')
    seal_number = models.CharField(max_length=32, blank=True, default='') # numero du scelle physique
    seal_code = models.CharField(max_length=32, blank=True, default='')
    unsealed = models.DateTimeField(blank=True, null=True)
    unsealed_by = models.CharField(max_length=64, blank=True, default='')
    # Reassort : items ajoutes sans verif complete. Le lot est signale « verif recommandee » (orange)
    # jusqu'a la prochaine verif.
    verif_recommended = models.BooleanField(default=False)
    restocked = models.DateTimeField(blank=True, null=True)
    restocked_by = models.CharField(max_length=64, blank=True, default='')
    restocked_count = models.PositiveIntegerField(default=0)  # items ajoutes depuis la derniere verif
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

    def check_seal(self, code) -> bool:
        return self.is_sealed and keys_match(self.seal_code, code)

    def __str__(self):
        return f"{self.name} ({self.id})"


PIN_RE = re.compile(r'^\d{4,8}$')
PIN_MAX_FAILURES = 5              # essais faux consecutifs avant blocage
PIN_LOCK_DURATION = timedelta(minutes=5)


class Role(models.TextChoices):
    NORMAL = 'normal', 'Secouriste'
    GESTION = 'gestion', 'Gestion'   # consultation et gestion de l'inventaire, sans les reglages du front
    ADMIN = 'admin', 'Administrateur'


class Secouristes(models.Model):
    matricule = models.CharField(max_length=16, primary_key=True, editable=False)
    nom = models.CharField(max_length=32)
    prenom = models.CharField(max_length=32)
    key = models.CharField(max_length=32, blank=True, null=True)
    key_expires = models.DateField(blank=True, null=True)
    role = models.CharField(max_length=8, choices=Role.choices, default=Role.NORMAL)
    active = models.BooleanField(default=True)
    created = models.DateTimeField(default=timezone.now)
    # PIN de connexion (4 a 8 chiffres, hache) : obligatoire pour les admins, facultatif sinon
    pin_hash = models.CharField(max_length=128, blank=True, default='')
    pin_failures = models.PositiveIntegerField(default=0)
    pin_locked_until = models.DateTimeField(blank=True, null=True)

    @property
    def has_pin(self) -> bool:
        return bool(self.pin_hash)

    @property
    def pin_required(self) -> bool:
        return self.has_pin or self.role == Role.ADMIN

    def set_pin(self, pin):
        """pin vide : supprime le PIN (refuse pour un admin). Leve ValueError si le format est invalide."""
        pin = str(pin or '').strip()
        if not pin:
            if self.role == Role.ADMIN:
                raise ValueError('Le PIN est obligatoire pour un administrateur')
            self.pin_hash = ''
        elif not PIN_RE.match(pin):
            raise ValueError('Le PIN doit comporter 4 à 8 chiffres')
        else:
            self.pin_hash = make_password(pin)
        self.pin_failures = 0
        self.pin_locked_until = None

    def pin_locked(self) -> bool:
        return self.pin_locked_until is not None and self.pin_locked_until > timezone.now()

    def check_pin(self, pin) -> bool:
        """Verifie le PIN et compte les echecs (blocage temporaire apres PIN_MAX_FAILURES)."""
        if self.pin_locked() or not self.pin_hash:
            return False
        if check_password(str(pin or ''), self.pin_hash):
            if self.pin_failures:
                self.pin_failures = 0
                self.save(update_fields=['pin_failures'])
            return True
        self.pin_failures += 1
        if self.pin_failures >= PIN_MAX_FAILURES:
            self.pin_failures = 0
            self.pin_locked_until = timezone.now() + PIN_LOCK_DURATION
        self.save(update_fields=['pin_failures', 'pin_locked_until'])
        return False

    @property
    def privileged(self) -> bool:
        """Mode privilegie du front (gestion ou admin)."""
        return self.role in (Role.GESTION, Role.ADMIN)

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


class NotificationSettings(models.Model):
    """Reglages des notifications SMS (une seule ligne, pk=1)."""
    enabled = models.BooleanField(default=False)
    stock_low = models.BooleanField(default=True)        # stock d'un type sous son minimum
    verif_problem = models.BooleanField(default=True)    # verif de lot incomplete, perimes, disparus
    seal_broken = models.BooleanField(default=True)      # scelle d'un lot brise
    expired_daily = models.BooleanField(default=False)   # resume des lots contenant des perimes (commande check_alerts)

    @classmethod
    def get(cls):
        settings_row, _ = cls.objects.get_or_create(pk=1)
        return settings_row


class SmsRecipient(models.Model):
    """Destinataire de l'API SMS de Free Mobile (identifiant + cle d'identification de l'espace abonne)."""
    name = models.CharField(max_length=64)
    user = models.CharField(max_length=32)
    password = models.CharField(max_length=64)
    active = models.BooleanField(default=True)
    last_sent = models.DateTimeField(blank=True, null=True)
    last_status = models.CharField(max_length=128, blank=True, default='')

    def __str__(self):
        return self.name


class PushKeys(models.Model):
    """Cles VAPID du serveur pour les notifications web (une seule ligne, pk=1, generee au premier usage)."""
    private_key = models.TextField()                # PEM PKCS8 (P-256)
    public_key = models.CharField(max_length=128)   # point non compresse, base64url (applicationServerKey)
    created = models.DateTimeField(default=timezone.now)


class PushSubscription(models.Model):
    """Abonnement d'un navigateur aux notifications web de stock (admins uniquement)."""
    user = models.ForeignKey(Secouristes, on_delete=models.CASCADE, related_name='push_subscriptions')
    endpoint = models.URLField(max_length=1024, unique=True)
    p256dh = models.CharField(max_length=128)
    auth = models.CharField(max_length=64)
    stock_low = models.BooleanField(default=True)     # un type passe sous son minimum
    stock_empty = models.BooleanField(default=True)   # un type arrive a 0
    created = models.DateTimeField(default=timezone.now)
    last_sent = models.DateTimeField(blank=True, null=True)
    last_status = models.CharField(max_length=128, blank=True, default='')

    def __str__(self):
        return f'{self.user_id} : {self.endpoint[:48]}'
