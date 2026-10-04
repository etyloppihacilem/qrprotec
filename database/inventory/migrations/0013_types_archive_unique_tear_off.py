"""Types d'items et de lots archivables, lots uniques, étiquettes à déchirer et admin à contacter par défaut."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0012_order_due_notifications"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="archived",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="itemtype",
            name="tear_off",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="lottype",
            name="archived",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="lottype",
            name="unique",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="default_contact",
            field=models.BooleanField(default=False),
        ),
    ]
