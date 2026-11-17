from django.db import migrations, models


class Migration(migrations.Migration):
    """Impression lancee a distance par un admin (LabelPrint.launch_requested) : le poste l'imprime a son prochain
    passage, sans que personne n'appuie sur « Lancer l'impression »."""

    dependencies = [
        ("inventory", "0072_itemtype_is_group"),
    ]

    operations = [
        migrations.AddField(
            model_name="labelprint",
            name="launch_requested",
            field=models.DateTimeField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="labelprint",
            name="launch_requested_by",
            field=models.CharField(blank=True, default="", max_length=64),
        ),
    ]
