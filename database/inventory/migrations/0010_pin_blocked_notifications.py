"""Notifications des admins : PIN bloque (50 essais ou code oublie, une seule fois par blocage), renouvellement et
expiration des etiquettes privees de lot et des badges, types de notifications web par utilisateur."""

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
        migrations.AddField(
            model_name="notificationsettings",
            name="lot_key_renewed",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="lot_key_expiring",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="badge_renewed",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="badge_expiring",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="lot_key_renewed",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="lot_key_expiring",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="badge_renewed",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="badge_expiring",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="lots",
            name="key_expiry_stage",
            field=models.PositiveSmallIntegerField(default=0),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="key_expiry_stage",
            field=models.PositiveSmallIntegerField(default=0),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="push_disabled",
            field=models.JSONField(blank=True, default=list),
        ),
        migrations.AddField(
            model_name="pushsubscription",
            name="device",
            field=models.CharField(blank=True, default="", max_length=64),
        ),
        migrations.AddField(
            model_name="notificationsettings",
            name="key_expiry_warning_days",
            field=models.PositiveSmallIntegerField(blank=True, null=True),
        ),
    ]
