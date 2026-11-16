import django.utils.timezone
from django.db import migrations, models


class Migration(migrations.Migration):
    """Heure de reception des verifs (envoi d'une verif faite hors connexion). Les verifs existantes restent sans
    heure de reception (None) : ajout sans valeur par defaut, puis valeur par defaut pour les nouvelles."""

    dependencies = [
        ("inventory", "0070_checklist_date_verif_lots"),
    ]

    operations = [
        migrations.AddField(
            model_name="verifs",
            name="received",
            field=models.DateTimeField(blank=True, null=True),
        ),
        migrations.AlterField(
            model_name="verifs",
            name="received",
            field=models.DateTimeField(blank=True, default=django.utils.timezone.now, null=True),
        ),
    ]
