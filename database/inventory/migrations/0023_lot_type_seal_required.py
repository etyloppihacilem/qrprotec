"""Scellé obligatoire par type de lot : un lot ouvert reste rouge jusqu'à un nouveau scellé."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0022_verif_participants"),
    ]

    operations = [
        migrations.AddField(
            model_name="lottype",
            name="seal_required",
            field=models.BooleanField(default=False),
        ),
    ]
