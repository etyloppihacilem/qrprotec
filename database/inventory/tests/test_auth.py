"""Tests du back : premiere configuration, PIN, connexion par nom, badges, connexion temporaire."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    from inventory.models import Operation, Secouristes


class SetupTests(ApiTestCase):
    def test_setup_and_createadmin(self) -> None:
        from io import StringIO
        from django.core.management import call_command
        self.operator.delete()
        code, body = self.call('GET', '/api/setup/', session=False)
        self.assertTrue(body['needs_admin'])
        self.assertEqual(self.call('GET', '/api/setup/', local=False)[0], 404)
        out = StringIO()
        call_command('createadmin', 'R001', 'Melica', 'Hippolyte', stdout=out)
        self.assertIn('badge?m=R001&key=', out.getvalue())
        code, body = self.call('GET', '/api/setup/', session=False)
        self.assertFalse(body['needs_admin'])
        admin = Secouristes.objects.get(matricule='R001')
        self.assertTrue(admin.privileged)
        self.assertEqual(admin.role, 'admin')
        old_hash = admin.key_hash
        call_command('createadmin', 'R001', stdout=StringIO())
        self.assertNotEqual(Secouristes.objects.get(matricule='R001').key_hash, old_hash)
        # PIN lu sur l'entree standard (qrprotec-setup) : jamais dans la ligne de commande
        from unittest import mock
        with mock.patch('sys.stdin', StringIO('482100\n')):
            call_command('createadmin', 'R001', '--pin-stdin', stdout=StringIO())
        self.assertTrue(Secouristes.objects.get(matricule='R001').check_pin('482100'))


# PIN haches avec un algorithme rapide : les tests de blocage font une cinquantaine d'essais
@override_settings(PASSWORD_HASHERS=['django.contrib.auth.hashers.MD5PasswordHasher'])
class PinTests(ApiTestCase):
    def auth(self, user: Secouristes, **extra: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key, **extra}, local=False)

    def test_admin_sets_pin_at_first_login_then_needs_it(self) -> None:
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.save()
        code, body = self.auth(admin)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_setup_required'])
        code, body = self.auth(admin, new_pin='12')
        self.assertEqual(code, 400)
        code, body = self.auth(admin, new_pin='482100')
        self.assertEqual(code, 200)
        self.assertTrue(body['has_pin'])
        code, body = self.auth(admin)
        self.assertEqual((code, body.get('pin_required')), (403, True))
        code, body = self.auth(admin, pin='482100')
        self.assertEqual(code, 200)
        self.assertNotIn('nom', body)  # front web : jamais de nom de famille, le poste l'a
        # le jeton de session ouvre l'etat des stocks sans redemander le PIN
        badge = {'matricule': 'A001', 'key': admin.new_key}
        code, _ = self.call('POST', '/api/stock/summary/', {'user': badge}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/stock/summary/', {'user': {**badge, 'session': body['session']}}, local=False)
        self.assertEqual(code, 200)
        # un admin ne peut pas supprimer son PIN
        code, _ = self.call('PATCH', '/api/users/A001/', {'pin': ''})
        self.assertEqual(code, 400)

    def test_lockout_and_optional_pin(self) -> None:
        code, body = self.auth(self.user)
        self.assertEqual(code, 200)  # secouriste sans PIN
        self.assertFalse(body['pin_required'])
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        self.assertTrue(body['has_pin'])
        self.user.refresh_from_db()
        for _ in range(5):
            code, body = self.auth(self.user, pin='000000')
        self.assertTrue(body['pin_locked'])
        code, body = self.auth(self.user, pin='123456')  # bon PIN mais bloque
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_locked'])
        self.call('PATCH', '/api/users/M001/', {'pin': ''})  # suppression (debloque)
        self.user.refresh_from_db()
        self.assertEqual(self.auth(self.user)[0], 200)

    def fail_pin(self, user: Secouristes, times: int) -> tuple[int, dict[str, Any]]:
        for _ in range(times):
            Secouristes.objects.filter(pk=user.pk).update(pin_locked_until=None)  # saute le blocage de 5 min
            code, body = self.auth(user, pin='000000')
        return code, body

    def test_pin_blocked_after_failures_until_admin_reset(self) -> None:
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.set_pin('482100')
        admin.save()
        code, _ = self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'M001'})
        self.assertEqual(code, 400)  # l'admin a contacter doit etre un administrateur
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'A001'})
        self.assertEqual((body['pin_contact'], body['pin_contact_name']), ('A001', 'Min Ad'))
        code, session = self.auth(self.user, pin='123456')
        self.assertEqual(code, 200)

        # un PIN correct remet le compteur a zero
        self.fail_pin(self.user, 49)
        self.assertEqual(self.auth(self.user, pin='123456')[0], 200)
        code, body = self.fail_pin(self.user, 49)
        self.assertFalse(body.get('pin_blocked', False))
        code, body = self.fail_pin(self.user, 1)  # 50e echec
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        reset = body['pin_reset']
        self.assertIn('pinreset?m=M001&t=', reset['url'])
        self.assertEqual(reset['contact'], 'Min')  # front web : jamais de nom de famille
        self.assertNotIn('Dupont', reset['name'])
        # bloque : meme le bon PIN est refuse, et la session deja ouverte ne vaut plus rien
        code, body = self.auth(self.user, pin='123456')
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        Secouristes.objects.filter(pk=self.user.pk).update(role='gestion')
        badge = {'matricule': 'M001', 'key': self.user.new_key, 'session': session['session']}
        self.assertEqual(self.call('POST', '/api/stock/summary/', {'user': badge}, local=False)[0], 403)

        token = reset['url'].split('t=')[1]
        from urllib.parse import unquote
        token = unquote(token)
        request = {'matricule': 'M001', 'token': token}
        # deblocage : admin connecte (badge + session PIN) seulement
        self.assertEqual(self.call('POST', '/api/pin-reset/', request, local=False)[0], 403)
        admin_badge = {'matricule': 'A001', 'key': admin.new_key}
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge}, local=False)
        self.assertEqual((code, body.get('pin_required')), (403, True))
        code, login = self.auth(admin, pin='482100')
        admin_badge['session'] = login['session']
        code, _ = self.call('POST', '/api/pin-reset/', {'matricule': 'M001', 'token': 'faux', 'user': admin_badge},
                            local=False)
        self.assertEqual(code, 404)
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge}, local=False)
        self.assertEqual((code, body['reset'], body['failures'], body['name']), (200, False, 50, 'Jeanne'))
        self.assertNotIn('nom', body)
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge, 'confirm': True}, local=False)
        self.assertEqual((code, body['reset']), (200, True))
        # lien a usage unique
        code, _ = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge, 'confirm': True}, local=False)
        self.assertEqual(code, 404)
        # l'utilisateur choisit un nouveau PIN a sa prochaine connexion
        code, body = self.auth(self.user)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_setup_required'])
        code, body = self.auth(self.user, new_pin='246800')
        self.assertEqual(code, 200)
        self.assertFalse(body['pin_reset_required'])
        self.assertEqual(self.auth(self.user, pin='246800')[0], 200)

    def test_local_pin_reset(self) -> None:
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        self.user.refresh_from_db()
        self.fail_pin(self.user, 50)
        code, users = self.call('GET', '/api/users/')
        row = next(user for user in users if user['matricule'] == 'M001')
        self.assertTrue(row['pin_blocked'])
        self.assertEqual(row['pin_failures'], 50)
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_reset': True})
        self.assertEqual((body['pin_blocked'], body['pin_reset_required'], body['has_pin']), (False, True, False))
        self.assertTrue(self.auth(self.user)[1]['pin_setup_required'])


    def test_forgotten_pin(self) -> None:
        # sans PIN : rien a oublier
        code, body = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 400)
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        code, _ = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': 'faux'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        self.assertTrue(body['pin_reset']['forgotten'])
        self.assertFalse(body['pin_reset']['notified'])  # aucune notification configuree
        self.assertIn('oublié', body['error'])
        # bloque comme apres 50 essais, meme lien de deblocage
        code, again = self.auth(self.user, pin='123456')
        self.assertTrue(again['pin_blocked'])
        self.assertEqual(again['pin_reset']['url'], body['pin_reset']['url'])
        code, users = self.call('GET', '/api/users/')
        row = next(user for user in users if user['matricule'] == 'M001')
        self.assertTrue(row['pin_forgotten'])
        self.call('PATCH', '/api/users/M001/', {'pin_reset': True})
        self.user.refresh_from_db()
        self.assertFalse(self.user.pin_forgotten)
        self.assertTrue(self.auth(self.user)[1]['pin_setup_required'])


class PinModeTests(ApiTestCase):
    """PIN obligatoire ou desactive par secouriste, et PIN obligatoire par defaut (reglage du serveur)."""

    def auth(self, user: Secouristes, **extra: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key, **extra}, local=False)

    def test_required_then_disabled(self) -> None:
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'required'})
        self.assertEqual((code, body['pin_mode'], body['pin_required']), (200, 'required', True))
        code, body = self.auth(self.user)
        self.assertEqual((code, body.get('pin_setup_required')), (403, True))
        self.assertEqual(self.auth(self.user, new_pin='246800')[0], 200)
        self.assertEqual(self.auth(self.user)[0], 403)
        # desactive : PIN supprime, plus demande
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual((code, body['has_pin'], body['pin_required']), (200, False, False))
        self.assertEqual(self.auth(self.user)[0], 200)
        # refuse pour un admin, et un mode inconnu
        self.assertEqual(self.call('PATCH', '/api/users/P001/', {'pin_mode': 'disabled'})[0], 400)
        self.assertEqual(self.call('PATCH', '/api/users/M001/', {'pin_mode': 'parfois'})[0], 400)
        # un PIN choisi par un admin reactive le PIN
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '135700'})
        self.assertEqual((body['pin_mode'], body['pin_required']), ('', True))

    def test_server_roles(self) -> None:
        self.assertEqual(self.auth(self.user)[0], 200)
        code, body = self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['gestion']})
        self.assertEqual((code, body['pin_required_roles']), (200, ['gestion']))
        self.assertEqual(self.auth(self.user)[0], 200)  # role secouriste : code toujours facultatif
        code, body = self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['normal', 'gestion']})
        self.assertEqual(body['pin_required_roles'], ['normal', 'gestion'])
        code, body = self.auth(self.user)
        self.assertEqual((code, body.get('pin_setup_required'), body.get('pin_at_login_default')), (403, True, True))
        # desactive pour ce secouriste : le reglage du role ne s'applique plus
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual(self.auth(self.user)[0], 200)
        # role inconnu refuse, ancien reglage (une case pour tous) toujours accepte
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['chef', 'pompier']})[0], 400)
        code, body = self.call('PATCH', '/api/server-settings/', {'pin_required_default': True})
        self.assertEqual(body['pin_required_roles'], ['normal', 'chef', 'gestion'])
        # reservee aux admins
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'pin_required_roles': []},
                                   session=views.session_token(self.user))[0], 403)

    def test_pin_at_login(self) -> None:
        """Code obligatoire mais pas demande a chaque connexion : il ne sert qu'a la connexion sans badge."""
        self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['normal']})
        code, body = self.auth(self.user, new_pin='246800', pin_at_login=False)
        self.assertEqual((code, body['has_pin'], body['pin_at_login'], body['pin_required']), (200, True, False, False))
        self.assertEqual(self.auth(self.user)[0], 200)  # badge seul
        cache.clear()
        response = self.client.post('/api/auth/name/', json.dumps({'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '246800'}),
                                    content_type='application/json', REMOTE_ADDR='10.0.0.9')
        self.assertEqual(response.status_code, 200)
        # coche par un admin : redemande apres le badge
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_at_login': True})
        self.assertEqual((body['pin_at_login'], body['pin_required']), (True, True))
        self.assertEqual(self.auth(self.user)[0], 403)
        # admin : toujours demande, case imposee
        self.operator.set_pin('482100')
        self.operator.save()
        code, body = self.call('PATCH', '/api/users/P001/', {'pin_at_login': False})
        self.assertEqual((body['pin_at_login'], body['pin_at_login_locked'], body['pin_required']), (True, True, True))
        # sans la case (ancien front) : demande a chaque connexion, comme avant
        self.call('PATCH', '/api/users/M001/', {'pin_reset': True})
        self.assertEqual(self.auth(self.user, new_pin='135700')[0], 200)
        self.assertEqual(self.auth(self.user)[0], 403)

    def test_defaults(self) -> None:
        """Nouvelle installation : code obligatoire pour tous les roles (decochable), code a la connexion obligatoire
        pour les admins seulement."""
        with real_pin_default():
            code, body = self.call('GET', '/api/server-settings/')
            self.assertEqual((body['pin_required_roles'], body['pin_login_roles']), (['normal', 'chef', 'gestion'], []))
            code, body = self.auth(self.user)  # premiere connexion : code a choisir, case cochee mais decochable
            self.assertEqual((code, body.get('pin_setup_required'), body.get('pin_at_login_default'),
                              body.get('pin_at_login_locked')), (403, True, True, False))
            code, body = self.auth(self.user, new_pin='246800', pin_at_login=False)
            self.assertEqual((code, body['pin_mandatory'], body['pin_can_disable'], body['pin_required']),
                             (200, True, False, False))
            self.assertEqual(self.auth(self.user)[0], 200)  # badge seul
            # role decoche : le code redevient facultatif
            self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['chef', 'gestion']})
            self.user.refresh_from_db()
            self.assertFalse(self.user.pin_mandatory)

    def test_login_roles(self) -> None:
        """Code a la connexion obligatoire pour un role : choisi a la premiere connexion, puis demande apres le badge a
        chaque connexion, case « Demander a chaque connexion » imposee."""
        code, body = self.call('PATCH', '/api/server-settings/', {'pin_login_roles': ['normal', 'admin']})
        self.assertEqual((code, body['pin_login_roles'], body['pin_required_roles']), (200, ['normal'], []))
        code, body = self.auth(self.user)
        self.assertEqual((code, body.get('pin_setup_required'), body.get('pin_at_login_locked')), (403, True, True))
        # la case decochee est ignoree
        code, body = self.auth(self.user, new_pin='246800', pin_at_login=False)
        self.assertEqual((code, body['pin_at_login'], body['pin_at_login_locked'], body['pin_mandatory']),
                         (200, True, True, True))
        self.assertEqual(self.auth(self.user)[0], 403)
        self.assertEqual(self.auth(self.user, pin='246800')[0], 200)
        # ni decochable, ni supprimable par l'utilisateur
        credentials = {'matricule': 'M001', 'key': self.user.new_key, 'session': views.session_token(self.user)}
        code, body = self.call('POST', '/api/me/pin/', {'user': credentials, 'pin_at_login': False}, local=False)
        self.assertEqual(code, 403)
        self.assertIn('TECH', body['error'])
        # un code deja choisi sans « chaque connexion » est redemande des que le role est coche
        self.call('PATCH', '/api/server-settings/', {'pin_login_roles': []})
        self.call('PATCH', '/api/users/M001/', {'pin_at_login': False})
        self.assertEqual(self.auth(self.user)[0], 200)
        self.call('PATCH', '/api/server-settings/', {'pin_login_roles': ['normal']})
        self.assertEqual(self.auth(self.user)[0], 403)
        # desactive pour ce secouriste : le reglage du role ne s'applique plus
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual((body['pin_at_login_locked'], body['pin_required']), (False, False))
        self.assertEqual(self.auth(self.user)[0], 200)
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'pin_login_roles': ['pompier']})[0], 400)

    def test_login_mode_per_user(self) -> None:
        """« Obligatoire a chaque connexion » pour un seul secouriste (pin_mode login)."""
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'login'})
        self.assertEqual((code, body['pin_mode'], body['pin_required'], body['pin_at_login_locked']),
                         (200, 'login', True, True))
        self.assertTrue(self.auth(self.user)[1]['pin_setup_required'])
        self.assertEqual(self.auth(self.user, new_pin='246800', pin_at_login=False)[0], 200)
        self.assertEqual(self.auth(self.user)[0], 403)
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': ''})
        self.assertEqual((body['pin_at_login_locked'], body['pin_mandatory']), (False, False))


class MyPinTests(ApiTestCase):
    """Code PIN choisi, change ou supprime par l'utilisateur dans ses reglages web."""

    def auth(self, user: Secouristes, **extra: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key, **extra}, local=False)

    def my_pin(self, user: Secouristes, **data: Any) -> tuple[int, Any]:
        credentials = {'matricule': user.matricule, 'key': user.new_key, 'session': views.session_token(user)}
        return self.call('POST', '/api/me/pin/', {'user': credentials, **data}, local=False)

    def test_set_change_then_disable(self) -> None:
        code, body = self.my_pin(self.user, pin='12')
        self.assertEqual(code, 400)
        code, body = self.my_pin(self.user, pin='246800')
        self.assertEqual((code, body['has_pin'], body['pin_required'], body['pin_can_disable']), (200, True, True, True))
        self.assertEqual(self.auth(self.user)[0], 403)
        self.assertEqual(self.auth(self.user, pin='246800')[0], 200)
        # changer ou supprimer : PIN actuel exige, essais faux comptes
        self.assertEqual(self.my_pin(self.user, pin='135700')[0], 403)
        self.assertEqual(self.my_pin(self.user, current_pin='000000', pin='135700')[0], 403)
        self.user.refresh_from_db()
        self.assertEqual(self.user.pin_failures, 2)
        self.assertEqual(self.my_pin(self.user, current_pin='246800', pin='135700')[0], 200)
        self.assertEqual(self.auth(self.user, pin='135700')[0], 200)
        code, body = self.my_pin(self.user, current_pin='135700', disable=True)
        self.assertEqual((code, body['has_pin'], body['pin_required'], body['pin_mode']), (200, False, False, ''))
        self.assertEqual(self.auth(self.user)[0], 200)
        self.assertEqual(self.my_pin(self.user, disable=True)[0], 400)
        self.assertTrue(Operation.objects.filter(summary__contains='code PIN supprimé').exists())
        # case « Demander a chaque connexion » : avec le PIN choisi, ou seule
        code, body = self.my_pin(self.user, pin='246800', pin_at_login=False)
        self.assertEqual((code, body['pin_at_login'], body['pin_required']), (200, False, False))
        self.assertEqual(self.auth(self.user)[0], 200)
        code, body = self.my_pin(self.user, pin_at_login=True)
        self.assertEqual((code, body['pin_required']), (200, True))
        self.assertEqual(self.auth(self.user)[0], 403)
        self.operator.set_pin('482100')
        self.operator.save()
        self.assertEqual(self.my_pin(self.operator, pin_at_login=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/me/pin/', {'pin': '246800'}, local=False)[0], 403)

    def test_disable_refused_when_mandatory(self) -> None:
        # admin : PIN obligatoire
        self.operator.set_pin('482100')
        self.operator.save()
        code, body = self.my_pin(self.operator, current_pin='482100', disable=True)
        self.assertEqual(code, 403)
        self.assertEqual(self.my_pin(self.operator, current_pin='482100', pin='975300')[0], 200)
        # PIN rendu obligatoire par un admin, pour ce secouriste ou pour tous
        self.my_pin(self.user, pin='246800')
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'required'})
        self.user.refresh_from_db()
        self.assertEqual(self.my_pin(self.user, current_pin='246800', disable=True)[0], 403)
        self.call('PATCH', '/api/users/M001/', {'pin_mode': ''})
        self.call('PATCH', '/api/server-settings/', {'pin_required_roles': ['normal']})
        code, body = self.auth(self.user, pin='246800')
        self.assertFalse(body['pin_can_disable'])
        self.assertEqual(self.my_pin(self.user, current_pin='246800', disable=True)[0], 403)
        # desactive par un admin : l'utilisateur peut en choisir un, puis le retirer
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.user.refresh_from_db()
        code, body = self.my_pin(self.user, pin='135700')
        self.assertEqual((code, body['pin_mode'], body['pin_can_disable']), (200, '', False))


class NameLoginTests(ApiTestCase):
    """Connexion sans badge par nom, prenom et PIN : ne dit jamais si le nom existe, essais limites."""

    def setUp(self) -> None:
        super().setUp()
        cache.clear()
        self.user.set_pin('246800')
        self.user.save()

    def login(self, nom: str = 'Dupont', prenom: str = 'Jeanne', pin: str = '246800', address: str = '10.0.0.1'
             ) -> tuple[int, dict[str, str]]:
        response = self.client.post('/api/auth/name/', json.dumps({'nom': nom, 'prenom': prenom, 'pin': pin}),
                                    content_type='application/json', REMOTE_ADDR=address)
        return response.status_code, response.json()

    def matricule_login(self, matricule: str = 'M001', pin: str | None = None, address: str = '10.0.0.1'
                       ) -> tuple[int, dict[str, Any]]:
        data = {'matricule': matricule, **({'pin': pin} if pin is not None else {})}
        response = self.client.post('/api/auth/', json.dumps(data), content_type='application/json', REMOTE_ADDR=address)
        return response.status_code, response.json()

    def test_matricule_without_badge_key_needs_pin(self) -> None:
        """Matricule seul (code sans l'URL du badge) : PIN toujours demande, refus qui ne dit pas si le matricule
        existe, essais comptes comme la connexion par nom."""
        for matricule in ('M001', 'X999'):
            code, body = self.matricule_login(matricule)
            self.assertEqual(code, 403)
            self.assertTrue(body['pin_required'])
        code, body = self.matricule_login(pin='246800')
        self.assertEqual((code, body['matricule']), (200, 'M001'))
        self.assertTrue(body['session'])
        code, wrong = self.matricule_login(pin='111100')
        code_unknown, unknown = self.matricule_login('X999', pin='111100')
        self.assertEqual((code, wrong['error']), (code_unknown, unknown['error']))
        self.assertTrue(wrong['pin_required'])
        self.user.refresh_from_db()
        self.assertEqual(self.user.name_login_failures, 1)
        self.assertEqual(self.user.pin_failures, 0)  # le PIN du badge n'est pas touche
        # sans PIN defini : impossible sans badge (il se choisit seulement avec la cle du badge)
        self.user.reset_pin()
        self.user.save()
        self.assertEqual(self.matricule_login(pin='246800')[0], 403)

    def test_login_and_session_as_badge(self) -> None:
        code, body = self.login(nom='DUPONT', prenom='jeanne')
        self.assertEqual((code, body['matricule']), (200, 'M001'))
        credentials = {'matricule': 'M001', 'session': body['session']}
        # le jeton remplace la cle du badge pour identifier les operations du telephone
        self.assertEqual(self.call('POST', '/api/verifs/recent/', {'user': credentials}, local=False)[0], 200)
        self.assertEqual(idendity.identity_from_request({'user': credentials}), 'M:M001')
        self.assertIsNone(idendity.identity_from_request({'user': {'matricule': 'M001', 'session': 'faux'}}))

    def test_compound_names_ignore_separators(self) -> None:
        """Tirets, espaces et apostrophes ne comptent pas : « Jean-Pierre Le Gall » se tape comme on veut."""
        self.user.prenom, self.user.nom = 'Jean-Pierre', "Le Gall-N'Diaye"
        self.user.save()
        for prenom, nom in (('jean pierre', 'le gall ndiaye'), ('Jean–Pierre', 'LE-GALL N’DIAYE'),
                            ('jeanpierre', 'legallndiaye'), ('Jéan - Pierre', "le gall-n'diaye")):
            self.assertEqual(self.login(nom=nom, prenom=prenom)[0], 200, (prenom, nom))
        self.assertEqual(self.login(nom='le gall', prenom='jean pierre')[0], 403)  # le nom reste complet

    def test_refusals_look_the_same(self) -> None:
        unknown = self.login(nom='Personne')
        wrong = self.login(pin='111100')
        self.assertEqual(unknown, wrong)
        self.assertEqual(unknown[0], 403)
        # sans PIN, ou PIN desactive : meme refus
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual(self.login(), unknown)

    def test_failures_pause_without_blocking_badge_pin(self) -> None:
        for _ in range(5):
            self.login(pin='111100', address='10.0.0.2')
        self.user.refresh_from_db()
        self.assertIsNotNone(self.user.name_login_locked_until)
        self.assertEqual(self.user.pin_failures, 0)  # le PIN du badge n'est pas touche
        self.assertEqual(self.login(address='10.0.0.3')[0], 403)  # en pause, meme avec le bon PIN
        # une connexion avec le badge leve la pause
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key,
                                                          'pin': '246800'}, local=False)[0], 200)
        self.assertEqual(self.login(address='10.0.0.3')[0], 200)

    def test_address_limit(self) -> None:
        for _ in range(10):
            self.login(nom='Personne', address='10.0.0.4')
        self.assertEqual(self.login(address='10.0.0.4')[0], 429)
        self.assertEqual(self.login(address='10.0.0.5')[0], 200)


class BadgeKeyTests(ApiTestCase):
    def test_badge_key_only_at_creation_and_renewal(self) -> None:
        code, created = self.call('POST', '/api/users/', {'matricule': 'M002', 'nom': 'Martin', 'prenom': 'Paul'})
        key = created['key']
        self.assertIn(f'key={key}', created['badge_url'])
        # ni la liste ni la fiche ne contiennent la cle, la base n'en garde que l'empreinte
        code, users = self.call('GET', '/api/users/')
        self.assertTrue(all('key' not in user and 'badge_url' not in user for user in users))
        code, body = self.call('GET', '/api/users/M002/')
        self.assertNotIn('key', body)
        stored = Secouristes.objects.get(matricule='M002')
        self.assertEqual(len(stored.key_hash), 64)
        self.assertNotIn(key, stored.key_hash)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': key}, local=False)[0], 200)
        # renouvellement : nouvelle cle affichee une fois, l'ancienne ne marche plus
        code, renewed = self.call('POST', '/api/users/M002/renew-key/')
        self.assertNotEqual(renewed['key'], key)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': key}, local=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': renewed['key']},
                                   local=False)[0], 200)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': None}, local=False)[0], 403)


class TempLoginTests(ApiTestCase):
    """QR code de connexion temporaire (outil de debug) : cree par un admin, valable 1h, session de 24h."""

    def admin(self) -> dict[str, str]:
        return {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}

    def qr(self, matricule: str = 'M001') -> tuple[int, dict[str, str]]:
        code, body = self.call('POST', '/api/debug/temp-login/', {'user': self.admin(), 'matricule': matricule},
                               local=False)
        return code, body

    def token(self, url: str) -> tuple[str, str]:
        from urllib.parse import parse_qs, urlparse
        query = parse_qs(urlparse(url).query)
        return query['m'][0], query['t'][0]

    def test_admin_creates_qr_and_rescuer_logs_in_for_24h(self) -> None:
        # refuse a un secouriste
        code, _ = self.call('POST', '/api/debug/temp-login/',
                            {'user': {'matricule': 'M001', 'key': self.user.new_key}, 'matricule': 'M001'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.qr()
        self.assertEqual(code, 200)
        self.assertIn('/badge?', body['url'])
        self.assertNotIn(self.user.new_key, body['url'])
        matricule, token = self.token(body['url'])
        self.assertEqual(matricule, 'M001')
        self.assertTrue(Operation.objects.filter(summary__contains='QR code de connexion temporaire').exists())
        code, user = self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, local=False)
        self.assertEqual(code, 200)
        self.assertTrue(user['temporary'])
        self.assertEqual(user['session_hours'], 24)
        self.assertNotIn('nom', user)
        # la session sert d'identifiant (comme apres une connexion par nom et PIN), pendant 24h et pas plus
        credentials = {'matricule': 'M001', 'session': user['session']}
        code, _ = self.call('POST', '/api/me/prefs/', {'user': credentials, 'web_prefs': {'scan_bubble': True}},
                            local=False)
        self.assertEqual(code, 200)
        self.assertIsNotNone(views.credentials_user(credentials))
        with mock.patch('django.core.signing.time.time', return_value=time.time() + 23 * 3600):
            self.assertIsNotNone(views.credentials_user(credentials))
        with mock.patch('django.core.signing.time.time', return_value=time.time() + 25 * 3600):
            self.assertIsNone(views.credentials_user(credentials))

    def test_qr_expires_after_one_hour_and_follows_the_badge(self) -> None:
        _, body = self.qr()
        matricule, token = self.token(body['url'])
        with mock.patch('django.core.signing.time.time', return_value=time.time() + 3700):
            code, _ = self.call('POST', '/api/auth/temp/', {'matricule': matricule, 'token': token}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/auth/temp/', {'matricule': 'P001', 'token': token}, local=False)
        self.assertEqual(code, 403)  # QR code d'un autre
        self.user.renew_key()  # nouveau badge : le QR code ne vaut plus
        self.user.save()
        code, _ = self.call('POST', '/api/auth/temp/', {'matricule': matricule, 'token': token}, local=False)
        self.assertEqual(code, 403)

    def test_never_for_an_admin(self) -> None:
        code, _ = self.qr('P001')
        self.assertEqual(code, 400)
        forged = views.temp_login_token(self.operator)
        code, _ = self.call('POST', '/api/auth/temp/', {'matricule': 'P001', 'token': forged}, local=False)
        self.assertEqual(code, 403)


class GuestCardTests(ApiTestCase):
    """Cartes d'invites : sans nom ni matricule, nom et prenom a la premiere connexion, 48h ensuite (modifiable),
    desactivables meme activees ; creees au poste (impression) ou depuis l'outil de debug du web (24h pour l'activer)."""

    def key_of(self, url: str) -> tuple[str, str]:
        from urllib.parse import parse_qs, urlparse
        query = parse_qs(urlparse(url).query)
        return query['m'][0], query['key'][0]

    def auth(self, matricule: str, key: str, **extra: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/auth/', {'matricule': matricule, 'key': key, **extra}, local=False)

    def admin(self) -> dict[str, str]:
        return {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}

    def test_printed_card_named_at_first_login_then_expires(self) -> None:
        from datetime import timedelta
        from unittest import mock
        code, body = self.call('POST', '/api/users/guests/', {'count': 3})
        self.assertEqual(code, 201)
        self.assertEqual(len(body['cards']), 3)
        card = body['cards'][0]
        self.assertTrue(card['matricule'].startswith('INV'))
        self.assertEqual((card['state'], card['hours'], card['created_by']), ('unused', 48, 'Admin Poste'))
        matricule, key = self.key_of(card['badge_url'])
        # pas dans la liste des secouristes, mais dans celle des cartes (sans cle)
        self.assertNotIn(matricule, [user['matricule'] for user in self.call('GET', '/api/users/')[1]])
        code, listed = self.call('GET', '/api/users/guests/')
        self.assertEqual((len(listed['cards']), listed['hours']), (3, 48))
        self.assertNotIn('badge_url', listed['cards'][0])
        # premiere connexion : nom et prenom demandes, pas de PIN
        code, body = self.auth(matricule, key)
        self.assertEqual((code, body.get('guest_name_required')), (403, True))
        code, body = self.auth(matricule, key, guest_nom=' Martin ', guest_prenom='Léa', guest_antenne='Lyon 3')
        self.assertEqual(code, 200)
        self.assertTrue(body['guest'])
        self.assertEqual(body['prenom'], 'Léa')
        self.assertFalse(body['pin_required'])
        self.assertIsNotNone(body['guest_expires'])
        stored = Secouristes.objects.get(matricule=matricule)
        self.assertEqual((stored.nom, stored.role, str(stored)), ('Martin', 'normal', 'Léa Martin (invité)'))
        # deuxieme connexion : plus de nom demande, le nom ne change pas
        code, body = self.auth(matricule, key, guest_nom='Autre', guest_prenom='Nom')
        self.assertEqual((code, body['prenom']), (200, 'Léa'))
        session = body['session']
        # pas de PIN a choisir dans les reglages
        code, _ = self.call('POST', '/api/me/pin/', {'user': {'matricule': matricule, 'session': session},
                                                    'pin': '123400'}, local=False)
        self.assertEqual(code, 403)
        # 48h plus tard : carte et session expirees
        later = timezone.now() + timedelta(hours=48, minutes=1)
        with mock.patch('django.utils.timezone.now', return_value=later):
            code, body = self.auth(matricule, key)
            self.assertEqual(code, 403)
            self.assertIn('déjà été activée par Léa de Lyon 3', body['error'])
            self.assertIsNone(views.session_user(session))
        # duree allongee par un admin : de nouveau valable
        code, row = self.call('PATCH', f'/api/users/{matricule}/', {'guest_hours': 72})
        self.assertEqual((code, row['hours']), (200, 72))
        with mock.patch('django.utils.timezone.now', return_value=later):
            self.assertEqual(self.auth(matricule, key)[0], 200)
        # jamais de role ni de PIN sur une carte d'invite
        self.assertEqual(self.call('PATCH', f'/api/users/{matricule}/', {'role': 'admin'})[0], 400)

    def test_disable_activated_card_cuts_session(self) -> None:
        code, body = self.call('POST', '/api/users/guests/', {'count': 1, 'guest_hours': 12})
        matricule, key = self.key_of(body['cards'][0]['badge_url'])
        self.assertEqual(body['cards'][0]['hours'], 12)
        code, body = self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa', guest_antenne='Lyon 3')
        session = body['session']
        self.assertIsNotNone(views.session_user(session))
        code, row = self.call('PATCH', f'/api/users/{matricule}/', {'active': False})
        self.assertEqual(row['state'], 'disabled')
        self.assertIsNone(views.session_user(session))
        code, body = self.auth(matricule, key)
        self.assertEqual(code, 403)
        # deja activee : elle n'est qu'a son invite, quelle que soit la raison du refus
        self.assertTrue(body['guest_used'])
        self.assertEqual(body['error'], "Cette carte a déjà été activée par Léa de Lyon 3, elle n'est plus "
                                        "utilisable par une autre personne. Utilisez une autre carte d'invité.")
        # une carte jamais utilisee se desactive aussi
        code, body = self.call('POST', '/api/users/guests/', {'count': 1})
        other, other_key = self.key_of(body['cards'][0]['badge_url'])
        self.call('PATCH', f'/api/users/{other}/', {'active': False})
        self.assertEqual(self.auth(other, other_key, guest_nom='A', guest_prenom='B', guest_antenne='C')[0], 403)
        self.assertEqual(self.call('POST', '/api/users/guests/', {'count': 1, 'guest_hours': 0})[0], 400)
        # reserve aux admins
        code, _ = self.call('GET', '/api/users/guests/', session=views.session_token(self.user))
        self.assertEqual(code, 403)

    def test_web_debug_card_must_be_activated_within_24h(self) -> None:
        from datetime import timedelta
        from unittest import mock
        code, _ = self.call('POST', '/api/debug/guest-cards/',
                            {'user': {'matricule': 'M001', 'key': self.user.new_key}, 'action': 'create'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/debug/guest-cards/', {'user': self.admin(), 'action': 'create'},
                               local=False)
        self.assertEqual(code, 200)
        card = body['card']
        self.assertEqual(card['created_by'], 'Admin')  # web : jamais de nom de famille
        self.assertIsNotNone(card['activate_before'])
        matricule, key = self.key_of(card['url'])
        late = timezone.now() + timedelta(hours=24, minutes=1)
        with mock.patch('django.utils.timezone.now', return_value=late):
            code, body = self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa', guest_antenne='Lyon 3')
            self.assertEqual(code, 403)
            self.assertIn('pas activée à temps', body['error'])
        code, body = self.call('POST', '/api/debug/guest-cards/', {'user': self.admin(), 'action': 'create'},
                               local=False)
        matricule, key = self.key_of(body['card']['url'])
        soon = timezone.now() + timedelta(hours=23)
        with mock.patch('django.utils.timezone.now', return_value=soon):
            code, body = self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa', guest_antenne='Lyon 3')
            self.assertEqual(code, 200)
        # activee : 48h a partir de l'activation, plus la limite de 24h
        with mock.patch('django.utils.timezone.now', return_value=soon + timedelta(hours=47)):
            self.assertEqual(self.auth(matricule, key)[0], 200)
        code, body = self.call('POST', '/api/debug/guest-cards/', {'user': self.admin()}, local=False)
        names = [row['name'] for row in body['cards']]
        self.assertIn('Léa (invité)', names)
        self.assertTrue(all(row['nom'] == '' for row in body['cards']))
        # desactivation depuis le web
        code, body = self.call('POST', '/api/debug/guest-cards/',
                               {'user': self.admin(), 'action': 'disable', 'matricule': matricule}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(self.auth(matricule, key)[0], 403)


class GuestCardReuseTests(ApiTestCase):
    """Numero d'une carte jamais activee repris, invite qui revient (reactivation), cartes jamais dans « À venir »,
    antenne dans le rapport d'etat."""

    def key_of(self, url: str) -> tuple[str, str]:
        from urllib.parse import parse_qs, urlparse
        query = parse_qs(urlparse(url).query)
        return query['m'][0], query['key'][0]

    def auth(self, matricule: str, key: str, **extra: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/auth/', {'matricule': matricule, 'key': key, **extra}, local=False)

    def new_card(self) -> tuple[str, str]:
        code, body = self.call('POST', '/api/users/guests/', {'count': 1})
        return self.key_of(body['cards'][0]['badge_url'])

    def test_antenna_required_and_in_report(self) -> None:
        matricule, key = self.new_card()
        code, body = self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa')
        self.assertEqual((code, body.get('guest_name_required')), (400, True))
        code, body = self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa', guest_antenne='Lyon 3')
        self.assertEqual((code, body['guest_antenna']), (200, 'Lyon 3'))
        from .. import lot_report
        self.assertEqual(lot_report.cited(f'M:{matricule}'), f"Léa Martin (carte {matricule}, invité de l'antenne Lyon 3)")
        self.assertEqual(lot_report.cited('M:M001'), 'Jeanne Dupont (M001)')
        # dans la suite du rapport : le nom seul
        self.assertEqual(lot_report.who(f'M:{matricule}'), 'Léa Martin')
        self.assertEqual(lot_report.who('M:M001'), 'Jeanne Dupont')

    def test_unused_card_number_reused_not_in_upcoming(self) -> None:
        matricule, key = self.new_card()
        # une carte d'invite n'est pas une echeance : jamais dans « À venir », quel que soit l'horizon
        for days in (30, 90, 365):
            code, body = self.call('GET', f'/api/events/?days={days}')
            self.assertFalse([row for row in body['events'] if 'guest' in row['kind']])
            self.assertFalse(body['groups'])
        code, row = self.call('POST', f'/api/users/guests/{matricule}/', {'action': 'reuse'})
        self.assertEqual(code, 200, row)
        same, new_key = self.key_of(row['badge_url'])
        self.assertEqual(same, matricule)
        self.assertEqual(self.auth(matricule, key, guest_nom='A', guest_prenom='B', guest_antenne='C')[0], 403)
        self.assertEqual(self.auth(matricule, new_key, guest_nom='A', guest_prenom='B', guest_antenne='C')[0], 200)
        # activee : plus de reprise du numero, plus dans « À venir »
        self.assertEqual(self.call('POST', f'/api/users/guests/{matricule}/', {'action': 'reuse'})[0], 400)

    def test_returning_guest_reactivated_with_new_card(self) -> None:
        from datetime import timedelta
        from unittest import mock
        matricule, key = self.new_card()
        self.auth(matricule, key, guest_nom='Martin', guest_prenom='Léa', guest_antenne='Lyon 3')
        week = timezone.now() + timedelta(days=7)
        with mock.patch('django.utils.timezone.now', return_value=week):
            self.assertEqual(self.auth(matricule, key)[0], 403)
            code, row = self.call('POST', f'/api/users/guests/{matricule}/',
                                  {'action': 'reactivate', 'guest_hours': 24, 'new_card': True})
            self.assertEqual(code, 200, row)
            self.assertEqual((row['state'], row['hours'], row['name']), ('active', 24, 'Léa Martin'))
            _, new_key = self.key_of(row['badge_url'])
            self.assertEqual(self.auth(matricule, key)[0], 403)  # ancienne carte remplacee
            code, body = self.auth(matricule, new_key)
            self.assertEqual((code, body['prenom']), (200, 'Léa'))
        with mock.patch('django.utils.timezone.now', return_value=week + timedelta(hours=25)):
            self.assertEqual(self.auth(matricule, new_key)[0], 403)
        # sans nouvelle carte : la meme carte remarche
        with mock.patch('django.utils.timezone.now', return_value=week + timedelta(days=1)):
            code, row = self.call('POST', f'/api/users/guests/{matricule}/', {'action': 'reactivate', 'guest_hours': 48})
            self.assertNotIn('badge_url', row)
            self.assertEqual(self.auth(matricule, new_key)[0], 200)
        # depuis le web : nouvelle carte a l'ecran
        admin = {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}
        code, body = self.call('POST', '/api/debug/guest-cards/',
                               {'user': admin, 'action': 'reactivate', 'matricule': matricule, 'guest_hours': 48},
                               local=False)
        self.assertEqual(code, 200, body)
        self.assertIn('/badge?', body['card']['url'])


class PinLengthTests(ApiTestCase):
    """PIN : 6 chiffres au moins pour un nouveau code, pas de vraie limite haute ; les anciens codes de 4 chiffres
    restent valables a la connexion."""

    def test_new_pin_six_digits_or_more_old_pins_still_work(self) -> None:
        from django.contrib.auth.hashers import make_password
        with self.assertRaises(ValueError):
            self.user.set_pin('12345')
        self.user.set_pin('12345678901234567890')  # plus de limite a 8 chiffres
        self.user.save()
        self.assertTrue(self.user.check_pin('12345678901234567890'))
        # ancien code de 4 chiffres (choisi avant le passage a 6) : la connexion par nom l'accepte toujours
        self.user.pin_hash = make_password('4821')
        self.user.save()
        code, body = self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '4821'},
                               local=False)
        self.assertEqual(code, 200, body)
        # mais on ne peut plus en choisir un de 4 chiffres
        credentials = {'matricule': 'M001', 'session': body['session']}
        code, body = self.call('POST', '/api/me/pin/', {'user': credentials, 'current_pin': '4821', 'pin': '1357'},
                               local=False)
        self.assertEqual(code, 400)
        self.assertIn('6 chiffres', body['error'])


class AuthJournalTests(ApiTestCase):
    """Journal des operations : moyen d'authentification de chaque operation (details.auth). Web : moyen de connexion ;
    poste principal : carte ou PIN ; poste distant : en plus le nom de sa cle API."""

    def report(self, extra: dict[str, str] | None, credentials: dict[str, str] | None = None, local: bool = True,
               session: str | None = None) -> str:
        body = {'lot': self.lot.id, 'comment': 'test', 'usable': True}
        if credentials:
            body['user'] = credentials
        if extra:
            response = self.client.post('/api/reports/', body, content_type='application/json', **extra)
            self.assertEqual(response.status_code, 201, response.content)
        else:
            code, data = self.call('POST', '/api/reports/', body, local=local, session=session)
            self.assertEqual(code, 201, data)
        return Operation.objects.filter(kind='report').latest('id').details.get('auth')

    def test_auth_method_in_journal(self) -> None:
        from ..models import FrontKey
        self.user.set_pin('482100')
        self.user.pin_at_login = False
        self.user.save()
        # web : carte seule (cle envoyee avec la requete), puis nom + PIN (jeton de session)
        self.assertEqual(self.report(None, {'matricule': 'M001', 'key': self.user.new_key}, local=False), 'Web : carte')
        _, user = self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '482100'},
                            local=False)
        self.assertEqual(self.report(None, {'matricule': 'M001', 'session': user['session']}, local=False),
                         'Web : nom + PIN')
        # poste principal (kiosk) : carte
        _, poste = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, session=False)
        self.assertEqual(self.report(None, session=poste['session']), 'Poste principal : carte')
        # poste distant : nom de la cle API en plus, connexion par nom + PIN
        _, key = FrontKey.create('Borne B')
        remote = {'qrprotec.role': 'remote', 'HTTP_X_QRPROTEC_KEY': key}
        response = self.client.post('/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '482100'},
                                    content_type='application/json', **remote)
        session = response.json()['session']
        self.assertEqual(self.report({**remote, 'HTTP_X_QRPROTEC_SESSION': session}),
                         'Poste distant (clé API « Borne B ») : nom + PIN')
        # operation ecrite pendant la connexion elle-meme : QR code de depannage du TECH
        token = views.temp_login_token(self.user)
        self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, local=False)
        self.assertEqual(Operation.objects.latest('id').details.get('auth'), 'Web : QR code de dépannage du TECH')


class LoginJournalTests(ApiTestCase):
    """Chaque connexion reussie est notee au journal (type « Connexion »), avec son moyen d'authentification : c'est
    l'historique des connexions de la page Utilisateurs du poste. Jamais annulable."""

    def logins(self) -> list[Operation]:
        return list(Operation.objects.filter(kind='login', by='M:M001').order_by('id'))

    def test_logins_recorded_with_method(self) -> None:
        from ..models import FrontKey
        self.user.set_pin('482100')
        self.user.pin_at_login = False
        self.user.save()
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 200)
        self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '482100'}, local=False)
        self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, session=False)
        _, key = FrontKey.create('Borne B')
        self.client.post('/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '482100'},
                         content_type='application/json', **{'qrprotec.role': 'remote', 'HTTP_X_QRPROTEC_KEY': key})
        # refusee : pas notee
        self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '999999'}, local=False)
        self.assertEqual([row.details.get('auth') for row in self.logins()], [
            'Web : carte', 'Web : nom + PIN', 'Poste principal : carte', 'Poste distant (clé API « Borne B ») : nom + PIN',
        ])
        # historique d'un utilisateur au poste : journal filtre par type et par personne
        code, data = self.call('GET', '/api/operations/?kind=login&by=M:M001&limit=2')
        self.assertEqual(code, 200, data)
        self.assertEqual(len(data['operations']), 2)
        self.assertTrue(data['more'])
        self.assertEqual(data['operations'][0]['details']['auth'], 'Poste distant (clé API « Borne B ») : nom + PIN')
        self.assertFalse(data['operations'][0]['undoable'])

    def test_login_never_cancellable(self) -> None:
        from .. import undo
        self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        login = self.logins()[-1]
        self.assertIsNone(login.batch)
        with self.assertRaisesMessage(ValueError, "Une connexion ne s'annule pas"):
            undo.plan(login)
