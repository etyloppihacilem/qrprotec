from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('inventory', '0043_antenna_lot_report'),
    ]

    operations = [
        migrations.AddField(
            model_name='lotrequirements',
            name='optional',
            field=models.BooleanField(default=False),
        ),
        migrations.AddField(
            model_name='lots',
            name='optional',
            field=models.BooleanField(default=False),
        ),
    ]
