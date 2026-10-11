"""PIN obligatoire ou desactive par secouriste, PIN obligatoire par defaut, connexion par nom et PIN."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0019_lot_key_location"),
    ]

    operations = [
        migrations.AddField(
            model_name="secouristes",
            name="name_login_failures",
            field=models.PositiveIntegerField(default=0),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="name_login_locked_until",
            field=models.DateTimeField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="secouristes",
            name="pin_mode",
            field=models.CharField(
                blank=True,
                choices=[
                    ("", "Réglage par défaut"),
                    ("required", "Obligatoire"),
                    ("disabled", "Désactivé"),
                ],
                default="",
                max_length=8,
            ),
        ),
        migrations.AddField(
            model_name="serversettings",
            name="pin_required_default",
            field=models.BooleanField(default=False),
        ),
    ]
