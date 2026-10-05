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
    path('pin-reset/', views.pin_reset),
    path('pin-forgot/', views.pin_forgot),
    path('items/<str:iid>/', views.item_detail),
    path('lots/summary/', views.lots_summary),   # avant lots/<id>/
    path('stock/summary/', views.stock_summary),
    path('stock/forecast/summary/', views.stock_forecast_summary),
    path('lots/<str:lot_id>/', views.lot_detail),
    path('lots/<str:lot_id>/verif/', views.lot_verif),
    path('verifs/', views.verifs),
    path('lots/<str:lot_id>/add/', views.lot_add_items),
    path('lots/<str:lot_id>/unseal/', views.lot_unseal),
    path('packs/<str:pack_id>/', views.sealed_pack_detail),
    path('remote-scanner/check/', views.remote_scanner_check),
    path('push/key/', views.push_key),
    path('push/subscription/', views.push_subscription),
    path('push/unsubscribe/', views.push_unsubscribe),
    path('push/test/', views.push_test),
    path('push/devices/', views.push_devices),
    path('push/devices/<int:device_id>/', views.push_device),
    path('wallet/', views.wallet_badge),
    path('wallet/apple/<str:token>.pkpass', views.wallet_apple_download),
]

# Servies uniquement par l'API locale (poste de gestion). Elles doivent preceder les routes publiques
# quand un meme prefixe est utilise (ex: items/batch/ avant items/<iid>/).
local_patterns = [
    path('item-types/', views.item_types),
    path('item-types/<str:type_code>/', views.item_type_detail),
    path('items/', views.items),
    path('items/batch/', views.items_batch),
    path('items/to-stock/', views.items_to_stock),
    path('items/<str:iid>/delete/', views.item_delete),
    path('items/<str:iid>/restore/', views.item_restore),
    path('stock/', views.stock),
    path('stock/forecast/', views.stock_forecast),
    path('stock/verif/', views.stock_verif),
    path('packs/', views.sealed_packs),
    path('packs/<str:pack_id>/open/', views.sealed_pack_open),
    path('packs/<str:pack_id>/close/', views.sealed_pack_close),
    path('lot-types/', views.lot_types),
    path('lot-types/<str:type_code>/', views.lot_type_detail),
    path('lot-types/<str:type_code>/requirements/', views.lot_type_requirements),
    path('lots/', views.lots),
    path('lots/<str:lot_id>/update/', views.lot_update),
    path('lots/<str:lot_id>/rotate-key/', views.lot_rotate_key),
    path('lots/<str:lot_id>/seal/', views.lot_seal),
    path('lots/<str:lot_id>/verifs/', views.lot_verifs),
    path('setup/', views.setup),
    path('users/', views.users),
    path('users/<str:matricule>/', views.user_detail),
    path('users/<str:matricule>/renew-key/', views.user_renew_key),
    path('server-settings/', views.server_settings),
    path('notifications/', views.notification_settings),
    path('notifications/recipients/', views.sms_recipients),
    path('notifications/recipients/<int:recipient_id>/', views.sms_recipient_detail),
    path('notifications/test/', views.sms_test),
    path('remote-scanner/', views.remote_scanner_sessions),
    path('remote-scanner/<str:session_id>/', views.remote_scanner_session),
]

# Front web mobile : repond aux URLs des QR codes (a la racine du domaine public)
web_patterns = [
    path('', web_views.page),
    path('verif', web_views.page),
    path('badge', web_views.page),
    path('pack', web_views.page),
    path('seal', web_views.page),
    path('pinreset', web_views.page),
    path('scanner', web_views.scanner_page),
    path('favicon.ico', web_views.favicon),
    path('web/<path:name>', web_views.asset),
]
