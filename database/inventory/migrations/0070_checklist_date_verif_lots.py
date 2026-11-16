import django.utils.timezone
from django.db import migrations, models


def dates_from_created(apps, schema_editor):
    """Listes existantes : leur date est celle de leur creation (pas la date de la migration)."""
    Checklist = apps.get_model('inventory', 'Checklist')
    for checklist in Checklist.objects.all().only('id', 'created'):
        Checklist.objects.filter(id=checklist.id).update(date=django.utils.timezone.localdate(checklist.created))


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0069_lots_storage_kind"),
    ]

    operations = [
        migrations.AddField(
            model_name="checklist",
            name="date",
            field=models.DateField(default=django.utils.timezone.localdate),
        ),
        migrations.AddField(
            model_name="checklist",
            name="verif_lots",
            field=models.JSONField(blank=True, default=list),
        ),
        migrations.RunPython(dates_from_created, migrations.RunPython.noop),
    ]
