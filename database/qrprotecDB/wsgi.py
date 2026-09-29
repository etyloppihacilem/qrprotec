"""
WSGI config for qrprotecDB project.

It exposes the WSGI callable as a module-level variable named ``application``.

For more information on this file, see
https://docs.djangoproject.com/en/6.1/howto/deployment/wsgi/
"""

import os

from django.core.wsgi import get_wsgi_application

os.environ.setdefault('DJANGO_SETTINGS_MODULE', 'qrprotecDB.settings')

application = get_wsgi_application()

# Deux points d'entree pour un serveur WSGI de production (gunicorn, uwsgi...) :
#   gunicorn -b 0.0.0.0:8000 qrprotecDB.wsgi:public_application
#   gunicorn -b 127.0.0.1:8001 qrprotecDB.wsgi:local_application
from inventory.middleware import RoleWSGIHandler  # noqa: E402

public_application = RoleWSGIHandler(application, 'public')
local_application = RoleWSGIHandler(application, 'local')
