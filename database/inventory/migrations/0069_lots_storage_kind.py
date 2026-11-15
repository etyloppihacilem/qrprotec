from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0068_item_split_sheet"),
    ]

    operations = [
        migrations.AddField(
            model_name="lots",
            name="storage_kind",
            field=models.CharField(blank=True, default="rangement", max_length=32),
        ),
    ]
