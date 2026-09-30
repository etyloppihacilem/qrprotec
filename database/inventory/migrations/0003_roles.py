# Roles : le booleen « privileged » devient un role (normal, gestion, admin).
# Les responsables existants deviennent administrateurs.

from django.db import migrations, models


def privileged_to_role(apps, schema_editor):
    Secouristes = apps.get_model('inventory', 'Secouristes')
    Secouristes.objects.filter(privileged=True).update(role='admin')


def role_to_privileged(apps, schema_editor):
    Secouristes = apps.get_model('inventory', 'Secouristes')
    Secouristes.objects.filter(role__in=['gestion', 'admin']).update(privileged=True)


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0002_seals_locations_sms"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="role",
            field=models.CharField(
                choices=[
                    ("normal", "Secouriste"),
                    ("gestion", "Gestion"),
                    ("admin", "Administrateur"),
                ],
                default="normal",
                max_length=8,
            ),
        ),
        migrations.RunPython(privileged_to_role, role_to_privileged),
        migrations.RemoveField(
            model_name="secouristes",
            name="privileged",
        ),
    ]
