# Notification web « Demande de connexion à l'application » (passkey par le navigateur, tous les roles)

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0085_label_loss"),
    ]

    operations = [
        migrations.AddField(
            model_name="pushsubscription",
            name="login_request",
            field=models.BooleanField(default=True),
        ),
    ]
