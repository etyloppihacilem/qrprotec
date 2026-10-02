"""Notification des admins quand un PIN est bloque (50 essais ou code oublie), une seule fois par blocage."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0009_badge_key_hash_pin_block"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="pin_forgotten",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_reset_notified",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="pin_blocked",
            field=models.BooleanField(default=True),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="pin_blocked",
            field=models.BooleanField(default=True),
        ),
    ]
