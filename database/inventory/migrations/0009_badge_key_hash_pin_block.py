"""Cles de badge hachees (SHA-256) et blocage du PIN leve par un administrateur.

Les badges deja imprimes restent valables : leur cle est hachee sur place.
"""

import hashlib

import django.db.models.deletion
from django.db import migrations, models


def hash_badge_keys(apps, schema_editor):
    Secouristes = apps.get_model('inventory', 'Secouristes')
    for user in Secouristes.objects.exclude(key__isnull=True).exclude(key=''):
        user.key_hash = hashlib.sha256(user.key.encode()).hexdigest()
        user.save(update_fields=['key_hash'])


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0008_front_keys"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="key_hash",
            field=models.CharField(blank=True, default="", max_length=64),
        ),
        migrations.RunPython(hash_badge_keys, migrations.RunPython.noop),
        migrations.RemoveField(
            model_name="secouristes",
            name="key",
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_failures_total",
            field=models.PositiveIntegerField(default=0),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_blocked",
            field=models.DateTimeField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_reset_required",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_contact",
            field=models.ForeignKey(
                blank=True, null=True, on_delete=django.db.models.deletion.SET_NULL, related_name="+",
                to="inventory.secouristes",
            ),
        ),
    ]
