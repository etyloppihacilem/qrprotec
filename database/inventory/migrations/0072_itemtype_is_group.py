from django.db import migrations, models


def mark_unit_groups(apps, schema_editor):
    """Les types a unites existants sont des types parents."""
    ItemType = apps.get_model('inventory', 'ItemType')
    ItemType.objects.exclude(unit='').update(is_group=True)


class Migration(migrations.Migration):
    """Type parent sans unite (ItemType.is_group, ex : « Bandes » rempli par « Bandes L » et « Bandes S »)."""

    dependencies = [
        ("inventory", "0071_verifs_received"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="is_group",
            field=models.BooleanField(default=False),
        ),
        migrations.RunPython(mark_unit_groups, migrations.RunPython.noop),
    ]
