from django.db import migrations, models


def default_to_roles(apps, schema_editor):
    """« PIN obligatoire par defaut » coche : le code devient obligatoire pour tous les roles."""
    ServerSettings = apps.get_model('inventory', 'ServerSettings')
    for row in ServerSettings.objects.filter(pin_required_default=True):
        row.pin_required_roles = ['normal', 'chef', 'gestion']
        row.save(update_fields=['pin_required_roles'])


def roles_to_default(apps, schema_editor):
    ServerSettings = apps.get_model('inventory', 'ServerSettings')
    for row in ServerSettings.objects.all():
        row.pin_required_default = bool(row.pin_required_roles)
        row.save(update_fields=['pin_required_default'])


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0048_user_web_prefs"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="pin_at_login",
            field=models.BooleanField(default=True),
        ),
        migrations.AddField(
            model_name="serversettings",
            name="pin_required_roles",
            field=models.JSONField(blank=True, default=list),
        ),
        migrations.RunPython(default_to_roles, roles_to_default),
        migrations.RemoveField(
            model_name="serversettings",
            name="pin_required_default",
        ),
    ]
