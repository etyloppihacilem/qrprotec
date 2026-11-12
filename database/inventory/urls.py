# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          urls.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

from django.urls import path

from . import views, web_views

# Servies par l'API publique ET l'API locale. Les ecritures exigent une cle sur l'API publique.
public_patterns = [
    path('health/', views.health),
    path('auth/', views.auth),
    path('auth/name/', views.auth_name),
    path('auth/temp/', views.auth_temp),         # QR code de connexion temporaire (outil de debug des admins)
    path('pin-reset/', views.pin_reset),
    path('pin-forgot/', views.pin_forgot),
    path('me/pin/', views.my_pin),               # PIN choisi, change ou supprime dans les reglages web
    path('me/prefs/', views.my_prefs),           # reglages du front web (ligne d'info, explication de la vérif)
    path('items/out/', views.items_out),         # avant items/<iid>/
    path('items/<str:iid>/', views.item_detail),
    path('items/<str:iid>/revision/', views.item_revision),
    path('lots/summary/', views.lots_summary),
    path('lots/batch/', views.lots_batch),   # avant lots/<id>/
    path('lots/order/', views.lots_order),   # ordre choisi des lots (gestion, admin)
    path('stock/summary/', views.stock_summary),
    path('stock/forecast/summary/', views.stock_forecast_summary),
    path('tasks/summary/', views.tasks_summary),
    path('orders/', views.orders_create),
    path('orders/<int:order_id>/cancel/', views.orders_cancel),
    path('debug/codes/', views.debug_codes_list),  # admins seulement
    path('debug/temp-login/', views.debug_temp_login),  # admins seulement
    path('lots/<str:lot_id>/', views.lot_detail),
    path('lots/<str:lot_id>/sheet.pdf', views.lot_sheet_pdf),
    path('lots/<str:lot_id>/state-report/', views.lot_state_report),
    path('lots/<str:lot_id>/verif/', views.lot_verif),
    path('verifs/', views.verifs),
    path('verifs/recent/', views.verifs_recent),
    path('verifs/cache/', views.verifs_cache),  # cache de vérif des fronts (vérif hors connexion)
    path('verifs/<int:verif_id>/report/', views.verif_report),
    path('lots/<str:lot_id>/add/', views.lot_add_items),
    path('lots/<str:lot_id>/unseal/', views.lot_unseal),
    path('lots/<str:lot_id>/seal-open/', views.lot_seal_open),
    path('lots/<str:lot_id>/used/', views.lot_used),
    path('lots/<str:lot_id>/return/', views.lot_return),
    path('lots/<str:lot_id>/slots/<str:action>/', views.lot_slot_action),
    path('packs/<str:pack_id>/', views.sealed_pack_detail),
    path('remote-scanner/check/', views.remote_scanner_check),
    path('push/key/', views.push_key),
    path('push/subscription/', views.push_subscription),
    path('push/unsubscribe/', views.push_unsubscribe),
    path('push/test/', views.push_test),
    path('push/devices/', views.push_devices),
    path('push/devices/<int:device_id>/', views.push_device),
    path('checklists/', views.checklists_create),
    path('checklists/list/', views.checklists_list),
    path('checklists/options/', views.checklists_options),
    path('checklists/departure/', views.checklists_departure),
    path('checklists/users/', views.checklists_users),
    path('checklists/<str:checklist_id>/', views.checklist_action),
    path('checklists/<str:checklist_id>/<str:action>/', views.checklist_action),
    path('reports/', views.reports_create),
    path('reports/list/', views.reports_list),
    path('reports/<int:report_id>/', views.report_update),
    # etiquettes d'items demandees depuis le web (admins), imprimees au poste
    path('label-prints/', views.label_prints_create),
    path('label-prints/list/', views.label_prints_list),
    path('label-prints/<int:print_id>/cancel/', views.label_print_cancel),
]

# Servies uniquement par l'API locale (poste de gestion). Elles doivent preceder les routes publiques
# quand un meme prefixe est utilise (ex: items/batch/ avant items/<iid>/).
local_patterns = [
    path('label-prints/pending/', views.label_prints_poste),  # meme sans personne de connecte
    path('label-prints/<int:print_id>/print/', views.label_print_print),
    path('label-prints/send/', views.label_prints_send),  # front d'un autre ordinateur -> poste
    path('label-prints/<int:print_id>/detail/', views.label_print_detail),
    path('server-problem/', views.server_problem_report),  # kiosk distant : sauvegarde ou envoi en echec
    path('front-configs/', views.front_configs),
    path('front-configs/<str:name>/', views.front_config_detail),
    path('item-types/', views.item_types),
    path('item-types/<str:type_code>/', views.item_type_detail),
    path('items/', views.items),
    path('revisions/', views.revisions_due),
    path('items/batch/', views.items_batch),
    path('items/disable/', views.items_disable),
    path('receptions/', views.receptions),
    path('receptions/store/', views.reception_store),
    path('receptions/storage/', views.reception_storage),
    path('items/to-stock/', views.items_to_stock),
    path('items/<str:iid>/delete/', views.item_delete),
    path('items/<str:iid>/restore/', views.item_restore),
    path('stock/', views.stock),
    path('stock/forecast/', views.stock_forecast),
    path('stock/verif/', views.stock_verif),
    path('tasks/', views.tasks_list),
    path('packs/', views.sealed_packs),
    path('packs/<str:pack_id>/open/', views.sealed_pack_open),
    path('packs/<str:pack_id>/close/', views.sealed_pack_close),
    path('lot-types/', views.lot_types),
    path('lot-types/<str:type_code>/', views.lot_type_detail),
    path('lot-types/<str:type_code>/requirements/', views.lot_type_requirements),
    path('lot-types/<str:type_code>/coverage/', views.lot_type_coverage),
    path('lot-types/<str:type_code>/slots/', views.lot_type_slots),
    path('lots/', views.lots),
    path('lots/<str:lot_id>/update/', views.lot_update),
    path('lots/<str:lot_id>/content/', views.lot_content),
    path('lots/<str:lot_id>/distribution/', views.lot_distribution),
    path('lots/<str:lot_id>/rotate-key/', views.lot_rotate_key),
    path('lots/<str:lot_id>/seal/', views.lot_seal),
    path('lots/<str:lot_id>/verifs/', views.lot_verifs),
    path('operations/', views.operations),
    path('setup/', views.setup),
    path('users/', views.users),
    path('users/<str:matricule>/', views.user_detail),
    path('users/<str:matricule>/renew-key/', views.user_renew_key),
    path('server-settings/', views.server_settings),
    path('barcodes-printed/', views.barcodes_printed),
    path('print-counts/', views.print_counts_view),  # compteur d'impressions (consommables)
    path('consumables/', views.consumables_view),
    path('consumables/refs/', views.consumable_refs),
    path('consumables/refs/<int:ref_id>/', views.consumable_ref_detail),
    path('consumables/moves/', views.consumable_moves),
    path('consumables/moves/<int:move_id>/delete/', views.consumable_move_delete),
    path('me/scanner-volume/', views.my_scanner_volume),
    path('notifications/', views.notification_settings),
    # API SMS plus maintenue (voir notifications.py)
    # path('notifications/recipients/', views.sms_recipients),
    # path('notifications/recipients/<int:recipient_id>/', views.sms_recipient_detail),
    # path('notifications/test/', views.sms_test),
    path('remote-scanner/', views.remote_scanner_sessions),
    path('remote-scanner/<str:session_id>/', views.remote_scanner_session),
]

# Front web mobile : repond aux URLs des QR codes (a la racine du domaine public)
web_patterns = [
    path('', web_views.page),
    path('verif', web_views.page),
    path('badge', web_views.page),
    path('item', web_views.page),
    path('task', web_views.page),
    path('liste', web_views.page),
    path('pack', web_views.page),
    path('seal', web_views.page),
    path('unseal', web_views.page),
    path('pinreset', web_views.page),
    path('scanner', web_views.scanner_page),
    path('favicon.ico', web_views.favicon),
    path('robots.txt', web_views.robots_txt),
    path('front-config/<str:token>', views.front_config_download),
    path('web/<path:name>', web_views.asset),
]
