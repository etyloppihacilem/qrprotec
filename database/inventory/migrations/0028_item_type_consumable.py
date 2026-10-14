"""Types d'items non consommables : absents à la vérif d'un lot, ils passent « hors lot » au lieu d'être perdus."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0027_problem_reports"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="consumable",
            field=models.BooleanField(default=True),
        ),
        migrations.AlterField(
            model_name="items",
            name="status",
            field=models.CharField(
                choices=[("active", "Present"), ("missing", "Disparu"), ("replaced", "Remplace"),
                         ("deleted", "Supprime manuellement"), ("out", "Sorti"), ("loose", "Hors lot")],
                default="active", max_length=8),
        ),
    ]
