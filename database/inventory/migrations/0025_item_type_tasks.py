"""Tâches par type d'item : l'item n'est vérifié qu'une fois toutes ses tâches cochées (étiquette de tâches)."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0024_stock_lot"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="tasks",
            field=models.JSONField(blank=True, default=list),
        ),
    ]
