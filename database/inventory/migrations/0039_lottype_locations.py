from django.db import migrations, models

from inventory.models import name_key


def fill_locations(apps, schema_editor):
    """Ordre de depart : celui d'avant (emplacements par ordre alphabetique, sans accents ni casse)."""
    LotType = apps.get_model('inventory', 'LotType')
    LotRequirements = apps.get_model('inventory', 'LotRequirements')
    for lot_type in LotType.objects.all():
        labels = {}
        for location in LotRequirements.objects.filter(lot_type=lot_type).values_list('location', flat=True):
            location = ' '.join(location.split())
            if location:
                labels.setdefault(name_key(location), location)
        if labels:
            lot_type.locations = [labels[key] for key in sorted(labels)]
            lot_type.save(update_fields=['locations'])


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0038_checklist_members_role_chef"),
    ]

    operations = [
        migrations.AddField(
            model_name="lottype",
            name="locations",
            field=models.JSONField(blank=True, default=list),
        ),
        migrations.RunPython(fill_locations, migrations.RunPython.noop),
    ]
