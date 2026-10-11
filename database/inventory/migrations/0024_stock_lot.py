"""Le stock : lot global de tous les rangements du stock, pour vérifier plusieurs rangements à la suite.

Crée le lot du stock (type « stock0 », rangement, lot unique, étiquette clé sans expiration) et y range les
rangements existants qui n'étaient pas déjà dans un autre rangement. Un lot ordinaire rangé dans un rangement du
stock en sort (lot indépendant) : seuls des rangements font partie du stock.
"""

from django.db import migrations

from inventory.base62 import encode_base62
from inventory.models import STOCK_LOT_NAME, STOCK_LOT_TYPE, generate_key, stock_lot_name


def create_stock(apps, schema_editor):
    LotType = apps.get_model('inventory', 'LotType')
    LotSequence = apps.get_model('inventory', 'LotSequence')
    Lots = apps.get_model('inventory', 'Lots')
    lot_type, created = LotType.objects.get_or_create(type=STOCK_LOT_TYPE, defaults={
        'name': STOCK_LOT_NAME, 'description': 'Lot global de tous les rangements du stock.',
        'storage': True, 'unique': True, 'created_by': 'qrprotec'})
    if not created:
        LotType.objects.filter(type=STOCK_LOT_TYPE).update(storage=True, unique=True, archived=False)
        lot_type.refresh_from_db()
    root = Lots.objects.filter(lot_type=lot_type).order_by('-active', 'created').first()
    if root is None:
        sequence, _ = LotSequence.objects.get_or_create(lot_type=lot_type)
        sequence.last_sequence += 1
        sequence.save()
        name = stock_lot_name(Lots.objects.filter(active=True).values_list('name', flat=True))
        root = Lots.objects.create(
            id=STOCK_LOT_TYPE + encode_base62(sequence.last_sequence, length=8), lot_type=lot_type,
            version=lot_type.version, verif_key=generate_key(), verif_key_expires=None, created_by='qrprotec',
            name=name, name_short=name[:16])
    else:
        Lots.objects.filter(id=root.id).update(active=True, parent=None, verif_key_expires=None)
    storage = Lots.objects.filter(lot_type__storage=True).exclude(id=root.id)
    storage.filter(parent__isnull=True).update(parent=root)
    storage.filter(parent__lot_type__storage=False).update(parent=root)
    Lots.objects.filter(lot_type__storage=False, parent__lot_type__storage=True).update(parent=None)


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0023_lot_type_seal_required"),
    ]

    operations = [
        migrations.RunPython(create_stock, migrations.RunPython.noop),
    ]
