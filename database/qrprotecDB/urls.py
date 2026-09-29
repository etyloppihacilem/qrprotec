"""URLs de l'API publique (exposee sur Internet en HTTP/HTTPS).

L'API locale (gestion) utilise qrprotecDB/urls_local.py, selectionne par
inventory.middleware.ApiRoleMiddleware selon le port qui a recu la requete.
"""
from django.urls import include, path

from inventory.urls import public_patterns, web_patterns

urlpatterns = [
    path('api/', include(public_patterns)),
    path('', include(web_patterns)),
]
