"""Items sortis du stock sans lot (statut « sorti »), et leurs mouvements et operations."""
from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("inventory", "0017_storage_lots_unique"),
    ]

    operations = [
        migrations.AlterField(
            model_name="itemmovement",
            name="kind",
            field=models.CharField(
                choices=[
                    ("move", "Déplacement"),
                    ("used", "Utilisé"),
                    ("discarded", "Jeté (périmé)"),
                    ("replaced", "Remplacé"),
                    ("deleted", "Supprimé"),
                    ("restored", "Restauré"),
                    ("out", "Sorti du stock"),
                ],
                max_length=10,
            ),
        ),
        migrations.AlterField(
            model_name="items",
            name="last_seen_while",
            field=models.CharField(
                choices=[
                    ("create", "Creation"),
                    ("verif", "Verification"),
                    ("add", "Ajout a un lot"),
                    ("remove", "Retour en stock"),
                    ("delete", "Suppression manuelle"),
                    ("restore", "Restauration"),
                    ("open", "Ouverture de paquet"),
                    ("out", "Sortie du stock"),
                ],
                max_length=8,
            ),
        ),
        migrations.AlterField(
            model_name="items",
            name="status",
            field=models.CharField(
                choices=[
                    ("active", "Present"),
                    ("missing", "Disparu"),
                    ("replaced", "Remplace"),
                    ("deleted", "Supprime manuellement"),
                    ("out", "Sorti"),
                ],
                default="active",
                max_length=8,
            ),
        ),
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
                    ("lot_key", "Étiquette privée renouvelée"),
                    ("catalog", "Types d'items et de lots"),
                    ("user", "Utilisateurs"),
                    ("settings", "Réglages"),
                ],
                db_index=True,
                max_length=16,
            ),
        ),
    ]
