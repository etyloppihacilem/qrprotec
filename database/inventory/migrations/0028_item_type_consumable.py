"""Types d'items non consommables : absents à la vérif d'un lot, ils repassent en stock au lieu d'être perdus."""

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
    ]
