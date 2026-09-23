# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          models.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Sep. 2026 at 18:37
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

from django.db import models
from django.db import transaction
from django.utils import timezone
from .base62 import encode_base62


# models.py
class ItemsPacksManager(models.Manager):
    def add_items(self, item_type, peremption, count, user, location=None, context=''):
        now = timezone.now()
        pack_id = f"{item_type.type}{peremption.strftime('%Y%m%d')}"

        with transaction.atomic():
            pack, _ = self.select_for_update().get_or_create(
                id=pack_id,
                defaults={'item_type': item_type, 'peremption': peremption, 'last_sequence': 0}
            )
            start = pack.last_sequence

            new_items = [
                Items(
                    iid=f"{pack_id}{encode_base62(start + i + 1)}",
                    pack=pack,
                    location=location,
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


class ItemType(models.Model):
    type = models.CharField(max_length=6, primary_key=True, editable=False)
    name = models.CharField(max_length=64)
    description = models.TextField()


class ItemsPacks(models.Model):
    id = models.CharField(max_length=14, primary_key=True, editable=False)
    item_type = models.ForeignKey(ItemType, on_delete=models.PROTECT, db_column="type", to_field="type")
    peremption = models.DateField(blank=True, null=True)
    last_sequence = models.PositiveIntegerField(default=0)
    objects = ItemsPacksManager()

    def save(self, *args, **kwargs):
        if not self.id:
            self.id = self._build_id()
        super().save(*args, **kwargs)

    def _build_id(self):
        return f"{self.item_type_id}{self.peremption.strftime("%Y%m%d")}"


class Items(models.Model):
    iid = models.CharField(max_length=20, primary_key=True, editable=False)
    pack = models.ForeignKey(ItemsPacks, on_delete=models.PROTECT, related_name="items")
    location = models.ForeignKey(
        'Lots',
        on_delete=models.SET_NULL,
        null=True,
        blank=True,
        related_name='items',
    )
    last_seen = models.DateTimeField()
    last_seen_by = models.CharField(max_length=32) # should be user id but could be something else
    last_seen_while = models.CharField(max_length=8) # TODO: certainement une enum...
    added = models.DateTimeField()
    added_by = models.CharField(max_length=32) # should be user as well (and admin)


class LotType(models.Model):
    type = models.CharField(max_length=6, primary_key=True)
    name = models.CharField(max_length=64)
    description = models.TextField()
    created = models.DateTimeField()
    created_by = models.CharField(max_length=32)
    version = models.PositiveIntegerField(default=0)
    valid_version = models.PositiveIntegerField(default=0)


class LotSequence(models.Model):
    lot_type = models.OneToOneField(LotType, on_delete=models.PROTECT, primary_key=True)
    last_sequence = models.PositiveIntegerField(default=0)


class LotRequirements(models.Model): # Pour mettre un item dans un lot
    lot_type = models.ForeignKey(LotType, on_delete=models.CASCADE, related_name="requirements")
    item_type = models.ForeignKey(ItemType, on_delete=models.PROTECT)


class Lots(models.Model):
    id = models.CharField(max_length=16, primary_key=True, editable=False)
    lot_type = models.ForeignKey(LotType, on_delete=models.PROTECT, db_column='type', to_field='type')
    version = models.PositiveIntegerField()
    verif_key = models.CharField(max_length=32)
    verif_key_expires = models.DateField()
    created = models.DateTimeField()
    created_by = models.CharField(max_length=32)
    last_used = models.DateTimeField()
    last_used_by = models.CharField(max_length=64)
    last_verif = models.DateTimeField()
    last_verif_by = models.CharField(max_length=64)
    last_verif_while = models.CharField(max_length=8)
    is_sealed = models.BooleanField(default=False)
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


class Secouristes(models.Model):
    matricule = models.CharField(max_length=16, primary_key=True, editable=False)
    nom = models.CharField(max_length=32)
    prenom = models.CharField(max_length=32)
    key = models.CharField(max_length=32, blank=True, null=True)
    key_expires = models.DateField(blank=True, null=True)


class Verifs(models.Model):
    lot = models.ForeignKey(Lots, on_delete=models.CASCADE)
    datetime = models.DateTimeField()
    by = models.CharField(max_length=64)


class VerifItem(models.Model):
    verif = models.ForeignKey(Verifs, on_delete=models.CASCADE)
    item = models.ForeignKey(Items, on_delete=models.CASCADE)
