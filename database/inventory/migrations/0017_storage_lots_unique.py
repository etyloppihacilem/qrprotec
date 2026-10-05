"""Rangements du stock : toujours des lots uniques, sans contenu attendu, étiquette privée sans expiration."""

from django.db import migrations, models
from django.db.models import Count

import inventory.models


def storage_rules(apps, schema_editor):
    LotType = apps.get_model('inventory', 'LotType')
    LotRequirements = apps.get_model('inventory', 'LotRequirements')
    Lots = apps.get_model('inventory', 'Lots')
    storage_types = LotType.objects.filter(storage=True)
    # un type de rangement qui a deja plusieurs lots (ancien « Tiroir ») les garde : il n'est juste plus possible
    # d'en creer d'autres
    storage_types.annotate(lot_total=Count('lots')).filter(lot_total__lte=1).update(unique=True)
    LotRequirements.objects.filter(lot_type__storage=True).delete()
    Lots.objects.filter(lot_type__storage=True).update(verif_key_expires=None, key_expiry_stage=0)


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0016_operations"),
    ]

    operations = [
        migrations.AlterField(
            model_name="lots",
            name="verif_key_expires",
            field=models.DateField(blank=True, default=inventory.models.default_lot_key_expiration, null=True),
        ),
        migrations.RunPython(storage_rules, migrations.RunPython.noop),
    ]
