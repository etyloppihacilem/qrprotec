from django.db import migrations, models


def record_current_versions(apps, schema_editor):
    """Verifs deja faites : comptees sur la version en cours de leur lot et de son type, pour que la prochaine
    modification du contenu cree bien une nouvelle version."""
    Verifs = apps.get_model('inventory', 'Verifs')
    for verif in Verifs.objects.filter(lot__isnull=False).select_related('lot__lot_type').iterator():
        verif.lot_type_version = verif.lot.lot_type.version
        verif.lot_version = verif.lot.version
        verif.save(update_fields=['lot_type_version', 'lot_version'])


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0053_departure_lists"),
    ]

    operations = [
        migrations.AddField(
            model_name="verifs",
            name="lot_type_version",
            field=models.PositiveIntegerField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="verifs",
            name="lot_version",
            field=models.PositiveIntegerField(blank=True, null=True),
        ),
        migrations.RunPython(record_current_versions, migrations.RunPython.noop),
    ]
