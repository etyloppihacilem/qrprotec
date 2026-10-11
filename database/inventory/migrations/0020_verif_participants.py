"""Vérif à plusieurs : autres participants enregistres sur la verif."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0019_lot_key_location"),
    ]

    operations = [
        migrations.AddField(
            model_name="verifs",
            name="participants",
            field=models.JSONField(blank=True, default=list),
        ),
    ]
