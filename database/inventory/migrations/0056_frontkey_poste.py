from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0055_server_problem"),
    ]

    operations = [
        migrations.AddField(
            model_name="frontkey",
            name="poste",
            field=models.BooleanField(default=False),
        ),
    ]
