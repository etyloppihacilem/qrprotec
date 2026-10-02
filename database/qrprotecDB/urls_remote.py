"""URLs de l'API distante (fronts d'autres machines, cle X-QRProtec-Key) : celles de l'API locale sans l'admin
Django. Les routes de gestion exigent en plus un utilisateur connecte (jeton X-QRProtec-Session)."""
from django.urls import include, path

from inventory.urls import local_patterns, public_patterns, web_patterns

urlpatterns = [
    path('api/', include(local_patterns + public_patterns)),
    path('', include(web_patterns)),
]
