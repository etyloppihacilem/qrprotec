from django.db import migrations, models
import django.db.models.deletion


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0039_lottype_locations"),
    ]

    operations = [
        migrations.AddField(
            model_name="lots",
            name="locations",
            field=models.JSONField(blank=True, default=list),
        ),
        migrations.CreateModel(
            name="LotContent",
            fields=[
                ("id", models.BigAutoField(auto_created=True, primary_key=True, serialize=False, verbose_name="ID")),
                ("extra", models.PositiveIntegerField(default=0)),
                ("location", models.CharField(blank=True, max_length=64, null=True)),
                ("item_type", models.ForeignKey(on_delete=django.db.models.deletion.PROTECT, to="inventory.itemtype")),
                ("lot", models.ForeignKey(on_delete=django.db.models.deletion.CASCADE, related_name="content",
                                          to="inventory.lots")),
            ],
            options={
                "unique_together": {("lot", "item_type")},
            },
        ),
    ]
