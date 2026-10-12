"""Volume de la douchette par utilisateur, et volume par défaut du serveur."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0025_item_type_tasks"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="scanner_volume",
            field=models.PositiveSmallIntegerField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="serversettings",
            name="scanner_volume_default",
            field=models.PositiveSmallIntegerField(default=4),
        ),
    ]
