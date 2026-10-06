"""Emplacement de l'etiquette privee dans le lot, affiche quand elle n'a pas ete scannee."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0018_item_out"),
    ]

    operations = [
        migrations.AddField(
            model_name="lots",
            name="key_location",
            field=models.CharField(blank=True, default="", max_length=128),
        ),
    ]
