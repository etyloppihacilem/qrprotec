"""Alerte « commande à passer » (prévisions de stock, commande check_alerts) : SMS et notifications web."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0011_item_movements"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="order_notified",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="order_due",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="order_due",
            field=models.BooleanField(default=True),
        ),
    ]
