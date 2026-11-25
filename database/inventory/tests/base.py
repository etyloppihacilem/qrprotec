"""Aides communes aux tests du back : ApiTestCase, PIN facultatif par defaut, journaux attendus."""

from __future__ import annotations

import importlib
import json
import math
import time
from unittest import mock
from datetime import date, datetime, timedelta
from decimal import Decimal
from io import StringIO
from pathlib import Path
from typing import Any, Literal, Type
from collections.abc import Callable

from django.apps import apps as django_apps

from django.conf import settings
from django.core.cache import cache
from django.core.management import call_command
from django.db import transaction
from django.test import TestCase, TransactionTestCase, override_settings
from django.utils import timezone

from .. import serializers as ser
from .. import models, views
from ..base62 import decode_base62
from .. import forecast
from .. import idendity
from .. import lot_sheet
from .. import services
from ..models import (
    ABSENCE_KINDS, ConsumableMove, ConsumableRef, PrintCount, PrintCountBatch, Checklist, ChecklistStatus, FrontKey, ItemMovement, ItemStatus, Items, ItemsPacks, ItemType,
    LotRequirements, Lots, LotType, MovementKind, Operation, OperationKind, Secouristes, STOCK_LOT_TYPE, VerifItem, Verifs,
    compact_iid, expand_iid,
)

LOCAL = {'qrprotec.role': 'local'}


class quiet_stderr:
    """Journaux attendus d'un test (erreur provoquee expres) : retire le handler stderr de LOGGING des loggers
    django et inventory le temps du bloc, sans toucher aux autres (server_problem, que certains tests verifient).
    Pour un simple message attendu, preferer assertLogs, qui le verifie en plus."""

    LOGGERS = ('django', 'inventory')

    def __enter__(self) -> quiet_stderr:
        import logging
        self.removed = []
        for name in self.LOGGERS:
            logger = logging.getLogger(name)
            for handler in list(logger.handlers):
                if type(handler) is logging.StreamHandler:
                    logger.removeHandler(handler)
                    self.removed.append((logger, handler))
        return self

    def __exit__(self, *exc: object) -> Literal[False]:
        for logger, handler in self.removed:
            logger.addHandler(handler)
        return False


# Code PIN obligatoire par defaut pour tous les roles (ServerSettings.pin_required_roles) : les tests sont ecrits
# pour un code facultatif, sauf ceux qui reglent pin_required_roles eux-memes ou qui verifient le vrai defaut
# (PinLoginRolesTests.test_defaults, avec real_pin_default).
_PIN_ROLES_FIELD = models.ServerSettings._meta.get_field('pin_required_roles')
_PIN_ROLES_DEFAULT = _PIN_ROLES_FIELD.default


def _set_pin_roles_default(default: Callable[[], list[str]]) -> None:
    _PIN_ROLES_FIELD.default = default
    _PIN_ROLES_FIELD.__dict__.pop('_get_default', None)  # cached_property de Django


def setUpModule() -> None:
    _set_pin_roles_default(list)


def tearDownModule() -> None:
    _set_pin_roles_default(_PIN_ROLES_DEFAULT)


class real_pin_default:
    """Le vrai defaut de pin_required_roles (tous les roles) le temps d'un bloc."""

    def __enter__(self) -> None:
        _set_pin_roles_default(_PIN_ROLES_DEFAULT)

    def __exit__(self, *exc: object) -> None:
        _set_pin_roles_default(list)


class ApiTestCase(TestCase):
    def call(self, method: str, path: str, data: Any = None, local: bool = True, session: str | bool | None = None
            ) -> Any:
        """local : requete du poste, avec par defaut la session de l'administrateur connecte (session=False :
        personne de connecte, ou un autre jeton)."""
        extra = dict(LOCAL) if local else {}
        if local and session is not False:
            extra['HTTP_X_QRPROTEC_SESSION'] = session or self.operator_session
        body = json.dumps(data) if data is not None else None
        response = self.client.generic(method, path, body or '', content_type='application/json', **extra)
        is_json = response.get('Content-Type', '').startswith('application/json')
        return response.status_code, (response.json() if is_json else None)

    def setUp(self) -> None:
        models.forget_code_prefix()  # code de l'antenne relu dans la base de ce test (aucun par defaut)
        self.addCleanup(models.forget_code_prefix)
        self.today = timezone.localdate()
        self.compresses = ItemType.objects.create(type='compre', name='Compresses', perissable=True, min_quantity=10)
        self.garrot = ItemType.objects.create(type='garrot', name='Garrot', perissable=False, min_quantity=2)
        self.lot_type = LotType.objects.create(type='sacpse', name='Sac PSE')
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.compresses, quantity=2)
        self.lot = Lots(lot_type=self.lot_type, name='Sac A', name_short='A', created_by='test')
        self.lot.save()
        self.user = Secouristes(matricule='M001', nom='Dupont', prenom='Jeanne')
        self.user.renew_key()
        self.user.save()
        # administrateur connecte sur le poste (jeton de session envoye par le front)
        self.operator = Secouristes(matricule='P001', nom='Poste', prenom='Admin', role='admin')
        self.operator.renew_key()
        self.operator.save()
        self.operator_session = views.session_token(self.operator)

    def create(self, item_type: ItemType, peremption: date | None, count: int) -> list[Items]:
        return ItemsPacks.objects.add_items(item_type, peremption, count, 'test')
