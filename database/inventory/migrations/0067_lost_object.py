from django.db import migrations, models

import inventory.models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0066_item_unit_groups"),
    ]

    operations = [
        migrations.AddField(
            model_name="itemtype",
            name="lost_object",
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name="serversettings",
            name="lost_object_message",
            field=models.TextField(blank=True, default=inventory.models.DEFAULT_LOST_OBJECT_MESSAGE),
        ),
    ]
