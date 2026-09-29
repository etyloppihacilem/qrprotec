"""URLs de l'API locale : administration complete, sans cle, reservee au front local."""
from django.contrib import admin
from django.urls import include, path

from inventory.urls import local_patterns, public_patterns, web_patterns

urlpatterns = [
    path('admin/', admin.site.urls),
    path('api/', include(local_patterns + public_patterns)),
    path('', include(web_patterns)),
]
