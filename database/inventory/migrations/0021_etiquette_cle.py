"""Journal des operations : l'etiquette privee s'appelle desormais etiquette cle (libelle seulement)."""

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0020_pin_mode"),
    ]

    operations = [
        migrations.AlterField(
            model_name="operation",
            name="kind",
            field=models.CharField(
                choices=[
                    ("verif", "Vérif"),
                    ("stock_verif", "Vérif du stock"),
                    ("restock", "Réassort"),
                    ("remove", "Retrait vers le stock"),
                    ("seal", "Scellage"),
                    ("unseal", "Ouverture de scellé"),
                    ("reception", "Réception"),
                    ("item_delete", "Suppression d'item"),
                    ("item_restore", "Restauration d'item"),
                    ("item_out", "Sortie du stock"),
                    ("item_out_used", "Sortie non revenue"),
                    ("pack_open", "Ouverture de paquet"),
                    ("pack_close", "Paquet refermé"),
                    ("lot", "Gestion des lots"),
                    ("lot_key", "Étiquette clé renouvelée"),
                    ("catalog", "Types d'items et de lots"),
                    ("user", "Utilisateurs"),
                    ("settings", "Réglages"),
                ],
                db_index=True,
                max_length=16,
            ),
        ),
    ]
