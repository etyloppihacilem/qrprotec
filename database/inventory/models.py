# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          models.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Sep. 2026 at 18:37
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

import hashlib
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


def hash_key(key: str) -> str:
    """Empreinte SHA-256 d'une cle aleatoire (badge, front distant) : seule l'empreinte est conservee."""
    return hashlib.sha256(key.encode()).hexdigest()


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
    # Rangement du stock (armoire, tiroir...) : les items ranges dans un lot de ce type restent en stock
    storage = models.BooleanField(default=False)

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


class KeyExpiry(models.IntegerChoices):
    """Etape d'expiration d'une cle (etiquette privee de lot, badge) deja annoncee aux admins : une seule alerte
    par etape, remise a zero au renouvellement."""
    VALID = 0, 'Valide'
    SOON = 1, 'Expire bientôt'
    EXPIRED = 2, 'Expirée'

    @classmethod
    def of(cls, expires, today, warning_days):
        if expires < today:
            return cls.EXPIRED
        if expires <= today + timedelta(days=warning_days):
            return cls.SOON
        return cls.VALID


def default_lot_key_expiration():
    return timezone.localdate() + timedelta(days=qrprotec_setting('LOT_KEY_VALIDITY_DAYS'))


class Lots(models.Model):
    id = models.CharField(max_length=16, primary_key=True, editable=False)
    lot_type = models.ForeignKey(LotType, on_delete=models.PROTECT, db_column='type', to_field='type')
    version = models.PositiveIntegerField()
    verif_key = models.CharField(max_length=32, default=generate_key)
    verif_key_expires = models.DateField(default=default_lot_key_expiration)
    key_expiry_stage = models.PositiveSmallIntegerField(default=0)  # alerte d'expiration envoyee (voir KeyExpiry)
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
    # Lot global (ex : un B+ compose d'un sac de soin et d'un sac d'O2, un VPS compose d'armoires et d'un B+).
    # Chaque sous-lot a ses propres etiquettes et se verifie seul ou avec les autres sous-lots du meme lot global.
    parent = models.ForeignKey('self', on_delete=models.SET_NULL, null=True, blank=True, related_name='children')

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
        self.key_expiry_stage = KeyExpiry.VALID

    def check_seal(self, code) -> bool:
        return self.is_sealed and keys_match(self.seal_code, code)

    @property
    def storage(self) -> bool:
        return self.lot_type.storage

    def ancestors(self):
        """Lots parents, du parent direct au lot global."""
        chain = []
        seen = {self.id}
        lot = self.parent
        while lot is not None and lot.id not in seen:
            chain.append(lot)
            seen.add(lot.id)
            lot = lot.parent
        return chain

    def root(self):
        chain = self.ancestors()
        return chain[-1] if chain else self

    def descendants(self, include_self=True):
        """Sous-lots actifs, en profondeur d'abord (ordre d'affichage), le lot lui-meme en tete."""
        by_parent = {}
        for lot in Lots.objects.select_related('lot_type').filter(active=True, parent__isnull=False):
            by_parent.setdefault(lot.parent_id, []).append(lot)
        for children in by_parent.values():
            children.sort(key=lambda child: (child.name.lower(), child.id))
        result = []
        seen = set()

        def walk(lot, depth):
            if lot.id in seen:
                return
            seen.add(lot.id)
            lot.depth = depth
            result.append(lot)
            for child in by_parent.get(lot.id, []):
                walk(child, depth + 1)

        walk(self, 0)
        return result if include_self else result[1:]

    def __str__(self):
        return f"{self.name} ({self.id})"


PIN_RE = re.compile(r'^\d{4,8}$')
PIN_MAX_FAILURES = 5              # essais faux consecutifs avant blocage
PIN_LOCK_DURATION = timedelta(minutes=5)
# Au-dela de QRPROTEC['PIN_BLOCK_AFTER_FAILURES'] echecs depuis le dernier PIN correct (50 par defaut), le PIN
# est bloque jusqu'a sa reinitialisation par un administrateur (lien envoye depuis le front web).


class Role(models.TextChoices):
    NORMAL = 'normal', 'Secouriste'
    GESTION = 'gestion', 'Gestion'   # consultation et gestion de l'inventaire, sans les reglages du front
    ADMIN = 'admin', 'Administrateur'


# Types de notifications web : libelle, roles qui peuvent les recevoir, valeur par defaut a l'abonnement.
# Un admin peut en couper certains par utilisateur (Secouristes.push_disabled, fenetre Utilisateurs).
_GESTION_ROLES = (Role.GESTION, Role.ADMIN)
_ADMIN_ROLES = (Role.ADMIN,)
PUSH_TYPES = {
    'stock_low': ('Stock bas (sous le minimum fixé)', _GESTION_ROLES, True),
    'stock_empty': ('Stock vide (0 en stock)', _GESTION_ROLES, True),
    'pin_blocked': ("PIN d'un utilisateur bloqué (lien de déblocage)", _ADMIN_ROLES, True),
    'lot_key_renewed': ("Étiquette privée d'un lot renouvelée", _GESTION_ROLES, False),
    'lot_key_expiring': ('Étiquette privée de lot qui expire bientôt ou a expiré', _GESTION_ROLES, False),
    'badge_renewed': ("Badge d'un utilisateur renouvelé", _ADMIN_ROLES, False),
    'badge_expiring': ('Badge qui expire bientôt ou a expiré', _ADMIN_ROLES, False),
}


class Secouristes(models.Model):
    matricule = models.CharField(max_length=16, primary_key=True, editable=False)
    nom = models.CharField(max_length=32)
    prenom = models.CharField(max_length=32)
    # Empreinte SHA-256 de la cle du badge : la cle n'est connue qu'a sa creation (impression du badge)
    key_hash = models.CharField(max_length=64, blank=True, default='')
    key_expires = models.DateField(blank=True, null=True)
    key_expiry_stage = models.PositiveSmallIntegerField(default=0)  # alerte d'expiration envoyee (voir KeyExpiry)
    role = models.CharField(max_length=8, choices=Role.choices, default=Role.NORMAL)
    active = models.BooleanField(default=True)
    created = models.DateTimeField(default=timezone.now)
    # PIN de connexion (4 a 8 chiffres, hache) : obligatoire pour les admins, facultatif sinon
    pin_hash = models.CharField(max_length=128, blank=True, default='')
    pin_failures = models.PositiveIntegerField(default=0)
    pin_locked_until = models.DateTimeField(blank=True, null=True)
    pin_failures_total = models.PositiveIntegerField(default=0)  # echecs depuis le dernier PIN correct
    pin_blocked = models.DateTimeField(blank=True, null=True)    # blocage leve seulement par un admin
    pin_reset_required = models.BooleanField(default=False)      # PIN reinitialise : a choisir a la connexion
    pin_forgotten = models.BooleanField(default=False)           # blocage demande par l'utilisateur (code oublie)
    pin_reset_notified = models.BooleanField(default=False)      # admins deja prevenus de ce blocage (une seule fois)
    # Administrateur a prevenir quand le PIN est bloque (affiche sur le telephone avec le lien de deblocage)
    pin_contact = models.ForeignKey('self', on_delete=models.SET_NULL, null=True, blank=True, related_name='+')
    # Types de notifications web coupes par un admin pour cet utilisateur (voir PUSH_TYPES)
    push_disabled = models.JSONField(default=list, blank=True)

    def push_role_types(self):
        """Types de notifications web que le role permet (gestion et admin seulement)."""
        return [name for name, (_, roles, _) in PUSH_TYPES.items() if self.role in roles]

    def push_types(self):
        """Types que cet utilisateur peut recevoir : ceux de son role, moins ceux qu'un admin a coupes."""
        if not self.active:
            return []
        disabled = set(self.push_disabled or [])
        return [name for name in self.push_role_types() if name not in disabled]

    @property
    def has_pin(self) -> bool:
        return bool(self.pin_hash)

    @property
    def pin_required(self) -> bool:
        return self.has_pin or self.role == Role.ADMIN or self.pin_reset_required

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
        self._clear_pin_failures()
        self.pin_reset_required = False

    def reset_pin(self):
        """Deblocage par un admin : plus de PIN, l'utilisateur en choisit un nouveau a sa prochaine connexion."""
        self.pin_hash = ''
        self._clear_pin_failures()
        self.pin_reset_required = True

    def forget_pin(self):
        """Code oublie : le PIN est bloque jusqu'a ce qu'un admin le reinitialise (meme lien que les 50 essais)."""
        if self.pin_blocked is None:
            self.pin_blocked = timezone.now()
            self.pin_forgotten = True
            self.pin_failures = 0
            self.pin_locked_until = None

    def _clear_pin_failures(self):
        self.pin_failures = 0
        self.pin_failures_total = 0
        self.pin_locked_until = None
        self.pin_blocked = None
        self.pin_forgotten = False
        self.pin_reset_notified = False

    def pin_locked(self) -> bool:
        return self.pin_locked_until is not None and self.pin_locked_until > timezone.now()

    def check_pin(self, pin) -> bool:
        """Verifie le PIN et compte les echecs : blocage temporaire apres PIN_MAX_FAILURES, puis blocage jusqu'a
        intervention d'un admin apres PIN_BLOCK_AFTER_FAILURES echecs depuis le dernier PIN correct."""
        if self.pin_blocked or self.pin_locked() or not self.pin_hash:
            return False
        if check_password(str(pin or ''), self.pin_hash):
            if self.pin_failures or self.pin_failures_total:
                self.pin_failures = 0
                self.pin_failures_total = 0
                self.save(update_fields=['pin_failures', 'pin_failures_total'])
            return True
        self.pin_failures += 1
        self.pin_failures_total += 1
        if self.pin_failures_total >= qrprotec_setting('PIN_BLOCK_AFTER_FAILURES'):
            self.pin_failures = 0
            self.pin_blocked = timezone.now()
        elif self.pin_failures >= PIN_MAX_FAILURES:
            self.pin_failures = 0
            self.pin_locked_until = timezone.now() + PIN_LOCK_DURATION
        self.save(update_fields=['pin_failures', 'pin_failures_total', 'pin_locked_until', 'pin_blocked'])
        return False

    @property
    def privileged(self) -> bool:
        """Mode privilegie du front (gestion ou admin)."""
        return self.role in (Role.GESTION, Role.ADMIN)

    def renew_key(self) -> str:
        """Nouvelle cle de badge, retournee en clair et gardee dans `new_key` le temps de la requete (reponse de
        creation ou de renouvellement, pour imprimer le badge). Seule son empreinte est enregistree."""
        self.new_key = generate_key()
        self.key_hash = hash_key(self.new_key)
        self.key_expires = timezone.localdate() + timedelta(days=qrprotec_setting('USER_KEY_VALIDITY_DAYS'))
        self.key_expiry_stage = KeyExpiry.VALID
        return self.new_key

    def badge_valid(self) -> bool:
        """Compte actif et badge non expire (la cle elle-meme est verifiee par check_key)."""
        return self.active and self.key_expires is not None and self.key_expires >= timezone.localdate()

    def check_key(self, key) -> bool:
        return (
            self.badge_valid()
            and isinstance(key, str)
            and keys_match(self.key_hash, hash_key(key))
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
    pin_blocked = models.BooleanField(default=True)      # PIN d'un utilisateur bloque (50 essais ou code oublie)
    lot_key_renewed = models.BooleanField(default=False)   # etiquette privee d'un lot renouvelee
    lot_key_expiring = models.BooleanField(default=False)  # etiquette privee qui expire bientot ou a expire (check_alerts)
    badge_renewed = models.BooleanField(default=False)     # badge d'un utilisateur renouvele
    badge_expiring = models.BooleanField(default=False)    # badge qui expire bientot ou a expire (check_alerts)
    # jours avant l'expiration pour l'alerte « expire bientot » (vide : QRPROTEC_KEY_EXPIRY_WARNING_DAYS)
    key_expiry_warning_days = models.PositiveSmallIntegerField(blank=True, null=True)

    @property
    def expiry_warning_days(self) -> int:
        if self.key_expiry_warning_days is None:
            return qrprotec_setting('KEY_EXPIRY_WARNING_DAYS')
        return self.key_expiry_warning_days

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
    """Abonnement d'un navigateur aux notifications web (roles gestion et admin), un par appareil."""
    user = models.ForeignKey(Secouristes, on_delete=models.CASCADE, related_name='push_subscriptions')
    endpoint = models.URLField(max_length=1024, unique=True)
    p256dh = models.CharField(max_length=128)
    auth = models.CharField(max_length=64)
    stock_low = models.BooleanField(default=True)     # un type passe sous son minimum
    stock_empty = models.BooleanField(default=True)   # un type arrive a 0
    pin_blocked = models.BooleanField(default=True)   # PIN d'un utilisateur bloque : lien de deblocage
    lot_key_renewed = models.BooleanField(default=False)
    lot_key_expiring = models.BooleanField(default=False)
    badge_renewed = models.BooleanField(default=False)
    badge_expiring = models.BooleanField(default=False)
    device = models.CharField(max_length=64, blank=True, default='')  # navigateur et systeme, d'apres le User-Agent
    created = models.DateTimeField(default=timezone.now)
    last_sent = models.DateTimeField(blank=True, null=True)
    last_status = models.CharField(max_length=128, blank=True, default='')

    def __str__(self):
        return f'{self.user_id} : {self.endpoint[:48]}'

    def wants(self, name):
        """Type choisi sur cet appareil et permis a son utilisateur."""
        return getattr(self, name) and name in self.user.push_types()


class FrontKey(models.Model):
    """Cle API d'un front distant (autre machine) : acces complet a l'API locale via l'API distante.

    Seule l'empreinte SHA-256 de la cle est conservee : la cle n'est affichee qu'a sa creation
    (`manage.py frontkey add NOM`).
    """
    PREFIX = 'qrpf_'

    name = models.CharField(max_length=64, unique=True)
    key_hash = models.CharField(max_length=64, unique=True)
    created = models.DateTimeField(default=timezone.now)
    last_used = models.DateTimeField(blank=True, null=True)
    last_address = models.CharField(max_length=64, blank=True, default='')
    revoked = models.BooleanField(default=False)

    @staticmethod
    def hash_key(key):
        return hash_key(key)

    @classmethod
    def create(cls, name):
        """Cree une cle et retourne (FrontKey, cle en clair)."""
        key = cls.PREFIX + secrets.token_urlsafe(32)
        return cls.objects.create(name=name, key_hash=cls.hash_key(key)), key

    @classmethod
    def authenticate(cls, key, address=''):
        """FrontKey valide correspondant a `key`, ou None. Note la derniere utilisation (au plus 1/min)."""
        if not key or not key.startswith(cls.PREFIX) or len(key) > 128:
            return None
        front = cls.objects.filter(key_hash=cls.hash_key(key), revoked=False).first()
        if front is None:
            return None
        now = timezone.now()
        if front.last_used is None or now - front.last_used > timedelta(minutes=1) or front.last_address != address:
            cls.objects.filter(pk=front.pk).update(last_used=now, last_address=address[:64])
        return front

    def __str__(self):
        return self.name
