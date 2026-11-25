"""Tests du back : front web (pages, fichiers servis, en-tetes)."""

from __future__ import annotations

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

from django.core.management.base import CommandError


class WebFrontTests(ApiTestCase):
    def test_qr_urls_serve_the_web_page(self) -> None:
        for url in ('/', '/verif', f'/verif?lot={self.lot.id}&key=x', '/badge?m=M001&key=x', '/pack?id=1',
                    '/item?id=compre20271231000000A1', '/task?id=compre20271231000000A1'):
            response = self.client.get(url)
            self.assertEqual(response.status_code, 200, url)
            self.assertIn(b'web/app/boot.js', response.content)
            self.assertEqual(response['Referrer-Policy'], 'no-referrer')

    def test_assets_whitelist(self) -> None:
        self.assertEqual(self.client.get('/web/app/boot.js').status_code, 200)
        self.assertEqual(self.client.get('/web/vendor/jsQR.js').status_code, 200)
        self.assertEqual(self.client.get('/web/../views.py').status_code, 404)
        self.assertEqual(self.client.get('/web/index.html').status_code, 404)

    def test_app_scripts_all_loaded(self) -> None:
        """Chaque script du front (web/app/*.js) est servi et charge par index.html, boot.js en dernier : il lance
        la page une fois toutes les fonctions declarees."""
        import re
        from pathlib import Path
        web = Path(__file__).resolve().parents[1] / 'web'
        loaded = re.findall(r'<script src="web/app/([\w-]+\.js)" defer></script>', (web / 'index.html').read_text(encoding='utf-8'))
        self.assertEqual(sorted(loaded), sorted(path.name for path in (web / 'app').glob('*.js')))
        self.assertEqual(loaded[-1], 'boot.js')
        for name in loaded:
            self.assertEqual(self.client.get(f'/web/app/{name}').status_code, 200, name)

    def test_assets_revalidated_with_etag(self) -> None:
        """Ressources en cache revalidees (304 sans corps) au lieu d'etre retelechargees a chaque ouverture."""
        response = self.client.get('/web/app/boot.js')
        etag, modified = response['ETag'], response['Last-Modified']
        self.assertEqual(self.client.get('/web/app/boot.js', HTTP_IF_NONE_MATCH=etag).status_code, 304)
        # ETag suffixe par la compression de Caddy
        self.assertEqual(self.client.get('/web/app/boot.js', HTTP_IF_NONE_MATCH=etag[:-1] + '-zstd"').status_code, 304)
        self.assertEqual(self.client.get('/web/app/boot.js', HTTP_IF_MODIFIED_SINCE=modified).status_code, 304)
        self.assertEqual(self.client.get('/web/app/boot.js', HTTP_IF_NONE_MATCH='"autre"').status_code, 200)
        # une autre date (nouvelle version, meme plus ancienne) : fichier renvoye
        self.assertEqual(self.client.get('/web/app/boot.js', HTTP_IF_MODIFIED_SINCE='Mon, 01 Jan 2035 00:00:00 GMT').status_code, 200)

    def test_pages_have_content_security_policy(self) -> None:
        for url in ('/', '/scanner?s=x&k=y'):
            policy = self.client.get(url)['Content-Security-Policy']
            self.assertIn("script-src 'self'", policy)
            self.assertIn("frame-ancestors 'none'", policy)
            self.assertIn('ws://testserver', policy)  # WebSockets du suivi en direct et de la douchette

    def test_camera_reader_served(self) -> None:
        """Lecteur limite au viseur dans les deux pages ; zxing-cpp (worker) et son module WebAssembly servis, le
        module versionne garde en cache, le nom attendu par decoder.js."""
        from pathlib import Path
        from ..web_views import WASM
        web = Path(__file__).resolve().parents[1] / 'web'
        for page in ('index.html', 'scanner.html'):
            self.assertIn('web/camera-scan.js', (web / page).read_text(encoding='utf-8'))
        for name in ('camera-scan.js', 'decoder.js', 'vendor/zxing-reader.js'):
            response = self.client.get('/web/' + name)
            self.assertEqual(response.status_code, 200, name)
            self.assertEqual(response['Cache-Control'], 'no-cache', name)
        response = self.client.get('/web/' + WASM)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response['Content-Type'], 'application/wasm')
        self.assertIn('immutable', response['Cache-Control'])
        self.assertEqual(b''.join(response.streaming_content)[:4], b'\0asm')
        self.assertIn(f"'{WASM}'", (web / 'decoder.js').read_text(encoding='utf-8'))

    def test_scanner_page_can_be_left(self) -> None:
        """Douchette : « ← Accueil » visible (l'id #home est cache par app.css, c'est la section d'accueil de
        index.html) et bouton « Quitter » qui marche hors connexion (scanner.js, leave)."""
        from pathlib import Path
        web = Path(__file__).resolve().parents[1] / 'web'
        html = (web / 'scanner.html').read_text(encoding='utf-8')
        self.assertNotIn('id="home"', html)
        self.assertIn('id="go-home"', html)
        self.assertIn('id="quit"', html)
        self.assertIn('id="left"', html)
        js = (web / 'scanner.js').read_text(encoding='utf-8')
        self.assertIn("$('#go-home').addEventListener('click', leave)", js)

    def test_douchette_easter_egg(self) -> None:
        """Le script est servi aux deux pages et ses phrases sont celles de l'app (codes.cpp)."""
        import re
        from pathlib import Path
        self.assertEqual(self.client.get('/web/douchette.js').status_code, 200)
        web = Path(__file__).resolve().parents[1] / 'web'
        for page in ('index.html', 'scanner.html'):
            self.assertIn('web/douchette.js', (web / page).read_text(encoding='utf-8'))
        js = (web / 'douchette.js').read_text(encoding='utf-8')
        js_phrases = [a or b for a, b in re.findall(r"^\s+(?:'([^']*)'|\"([^\"]*)\"),$", js, re.M)]
        self.assertGreater(len(js_phrases), 1)
        codes = Path(__file__).resolve().parents[3] / 'app' / 'src' / 'core' / 'codes.cpp'
        if not codes.exists():
            self.skipTest("sources de l'app absentes")
        cpp = codes.read_text(encoding='utf-8')
        block = cpp[cpp.index('douchette_phrases()'):]
        block = block[:block.index('};')]
        cpp_phrases = re.findall(r'^\s+"(.*)",$', block, re.M)
        self.assertEqual(js_phrases, cpp_phrases)

    def test_icons_and_service_worker(self) -> None:
        for url in ('/web/sw.js', '/web/icon-192.png', '/web/icon-512.png', '/web/favicon.png', '/favicon.ico',
                    '/web/icon-maskable-192.png', '/web/icon-maskable-512.png', '/web/apple-touch-icon.png'):
            self.assertEqual(self.client.get(url).status_code, 200, url)
        self.assertEqual(self.client.get('/favicon.ico')['Content-Type'], 'image/png')

    def test_app_service_worker_offline(self) -> None:
        """Appli ouverte sans reseau : service worker a la racine (portee = tout le site), version et liste des
        fichiers ecrites dedans, chaque fichier liste est servi, toutes les pages de l'appli couvertes."""
        import json
        import re
        from ..models import ServerSettings
        from ..web_views import ASSETS, PAGES
        response = self.client.get('/app-sw.js')
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response['Content-Type'].startswith('text/javascript'))
        self.assertEqual(response['Cache-Control'], 'no-cache')
        script = response.content.decode()
        self.assertNotIn('__VERSION__', script)
        files = json.loads(re.search(r'^const FILES = (.*);$', script, re.M).group(1))
        pages = json.loads(re.search(r'^const PAGES = (.*);$', script, re.M).group(1))
        version = re.search(r'^const VERSION = "([0-9a-f]+)";$', script, re.M).group(1)
        self.assertEqual(pages, list(PAGES))
        self.assertIn('./', files)
        self.assertNotIn('web/sw.js', files)  # service worker des notifications : a part
        self.assertEqual(set(files), {'./'} | {f'web/{name}' for name in ASSETS if name != 'sw.js'})
        for path in files:
            self.assertEqual(self.client.get('/' + path.removeprefix('./')).status_code, 200, path)
        for page in PAGES:
            self.assertEqual(self.client.get('/' + page).status_code, 200, page)
        # reglage qui change la page : nouvelle version, le telephone recharge tout
        settings = ServerSettings.get()
        settings.web_barcodes = not settings.web_barcodes
        settings.save()
        self.assertNotIn(version, self.client.get('/app-sw.js').content.decode())

    def test_robots_no_index(self) -> None:
        from pathlib import Path
        response = self.client.get('/robots.txt')
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response['Content-Type'].startswith('text/plain'))
        self.assertEqual(response.content.decode(), 'User-agent: *\nDisallow: /\n')
        # en-tete sur toutes les reponses : pages, fichiers du front, API, erreurs
        for url in ('/', '/item?k=x', '/scanner', '/web/app/boot.js', '/robots.txt', '/introuvable'):
            self.assertEqual(self.client.get(url)['X-Robots-Tag'], 'noindex, nofollow', url)
        for page in ('index.html', 'scanner.html'):
            html = (Path(__file__).resolve().parents[1] / 'web' / page).read_text(encoding='utf-8')
            self.assertIn('<meta name="robots" content="noindex, nofollow">', html)

    def test_manifest_installable(self) -> None:
        import json
        response = self.client.get('/web/manifest.webmanifest')
        self.assertEqual(response['Content-Type'], 'application/manifest+json')
        manifest = json.loads(b''.join(response.streaming_content))
        self.assertEqual(manifest['name'], 'QRprotec')
        self.assertEqual(manifest['short_name'], 'QRprotec')
        # chaque icone du manifeste est servie ; une maskable pour les icones rondes d'Android
        for icon in manifest['icons']:
            self.assertEqual(self.client.get('/web/' + icon['src']).status_code, 200, icon['src'])
        self.assertIn('maskable', {icon['purpose'] for icon in manifest['icons']})


class BarcodeLabelTests(ApiTestCase):
    """Etiquette a code-barres (Code 128, l'iid seul) choisie par type d'item, pour les objets ronds."""

    def test_item_type_label_style(self) -> None:
        code, body = self.call('POST', '/api/item-types/', {'type': 'flacon', 'name': 'Flacon', 'label_style': 'barcode'})
        self.assertEqual(code, 201)
        self.assertEqual(body['label_style'], 'barcode')
        flacon = ItemType.objects.get(type='flacon')
        item = self.create(flacon, None, 1)[0]
        Items.objects.filter(iid=item.iid).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        styles = {entry['iid']: entry['label_style'] for entry in body['items']}
        self.assertEqual(styles[item.iid], 'barcode')
        code, body = self.call('PATCH', '/api/item-types/flacon/', {'label_style': 'small'})
        self.assertEqual(body['label_style'], 'small')
        self.assertEqual(self.call('PATCH', '/api/item-types/flacon/', {'label_style': 'dm'})[0], 400)
        types = {entry['type']: entry for entry in self.call('GET', '/api/item-types/')[1]}
        self.assertEqual(types['flacon']['label_style'], 'small')

    def test_tear_off_and_tasks_exclusive(self) -> None:
        """Une etiquette a dechirer disparait a l'ouverture : pas de liste de taches dessus."""
        code, _ = self.call('POST', '/api/item-types/', {'type': 'sachet', 'name': 'Sachet', 'tear_off': True,
                                                         'tasks': ['Vérifier']})
        self.assertEqual(code, 400)
        self.assertEqual(self.call('POST', '/api/item-types/', {'type': 'sachet', 'name': 'Sachet', 'tear_off': True})[0], 201)
        self.assertEqual(self.call('PATCH', '/api/item-types/sachet/', {'tasks': ['Vérifier']})[0], 400)
        self.assertEqual(self.call('PATCH', '/api/item-types/sachet/', {'tear_off': False, 'tasks': ['Vérifier']})[0], 200)

    def test_web_barcodes_enabled_by_first_print(self) -> None:
        """Le decodeur n'est charge par les pages qu'une fois un code-barres imprime, desactivable par un admin."""
        for url in ('/', '/scanner?s=x&k=y'):
            self.assertNotIn(b'web/code128.js', self.client.get(url).content, url)
        self.assertFalse(self.call('GET', '/api/health/')[1]['web_barcodes'])
        self.assertEqual(self.call('POST', '/api/barcodes-printed/', {}, local=False)[0], 404)  # pas sur l'API publique
        self.assertEqual(self.call('POST', '/api/barcodes-printed/', {}, session=False)[0], 200)  # poste sans connecte
        self.assertTrue(self.call('GET', '/api/health/')[1]['web_barcodes'])
        for url in ('/', '/scanner?s=x&k=y'):
            self.assertIn(b'web/code128.js', self.client.get(url).content, url)
        code, body = self.call('PATCH', '/api/server-settings/', {'web_barcodes': False})
        self.assertFalse(body['web_barcodes'])
        self.assertNotIn(b'web/code128.js', self.client.get('/').content)

    def test_compact_iid(self) -> None:
        """Forme courte des etiquettes : D sans date, N pour les zeros de tete du compteur (au moins deux)."""
        self.assertEqual(compact_iid('serphy2026102900000007'), 'serphy20261029N7')
        self.assertEqual(compact_iid('lecgly0000000000000006'), 'lecglyDN6')
        self.assertEqual(compact_iid('abcdef202712310zzzzzzz'), 'abcdef202712310zzzzzzz')
        for iid in ('serphy2026102900000007', 'lecgly0000000000000006', 'abcdef202712310zzzzzzz', 'abcdef00000000N1234567'):
            self.assertEqual(expand_iid(compact_iid(iid)), iid)
        self.assertEqual(expand_iid('lecglyDN6'), 'lecgly0000000000000006')
        self.assertEqual(expand_iid('serphy2026N7'), '')
        item = self.create(self.compresses, self.today + timedelta(days=90), 1)[0]
        Items.objects.filter(iid=item.iid).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        entry = next(entry for entry in body['items'] if entry['iid'] == item.iid)
        self.assertEqual(entry['iid_short'], compact_iid(item.iid))
        self.assertTrue(entry['url'].endswith('item?id=' + compact_iid(item.iid)))

    def test_code_prefix(self) -> None:
        """Code de l'antenne devant les codes imprimes : enregistre au premier demarrage puis fige ; a la lecture, le
        notre est retire, une etiquette sans code (imprimee avant) est a nous, celle d'une autre antenne est refusee."""
        out = StringIO()
        with override_settings(QRPROTEC={**settings.QRPROTEC, 'CODE_PREFIX': 'PC'}):
            call_command('code_prefix', '--sync', stdout=out, stderr=StringIO())
        self.assertEqual(models.ServerSettings.get().code_prefix, 'PC')
        self.assertEqual(models.code_prefix(), 'PC')
        # fige : un autre code dans la configuration est ignore, il faut --force
        err = StringIO()
        with override_settings(QRPROTEC={**settings.QRPROTEC, 'CODE_PREFIX': '12'}):
            call_command('code_prefix', '--sync', stdout=StringIO(), stderr=err)
        self.assertIn('ignoré', err.getvalue())
        self.assertEqual(models.ServerSettings.get().code_prefix, 'PC')
        with self.assertRaises(CommandError):
            call_command('code_prefix', '12', stdout=StringIO())
        with self.assertRaises(CommandError):
            call_command('code_prefix', 'pc', '--force', stdout=StringIO())

        iid = 'serphy2026102900000007'
        self.assertEqual(models.item_code(iid), 'PCserphy20261029N7')
        self.assertEqual(models.read_iid('PCserphy20261029N7'), (iid, ''))
        self.assertEqual(models.read_iid('PCserphy2026102900000007'), (iid, ''))
        self.assertEqual(models.read_iid('serphy20261029N7'), (iid, ''))  # ancienne etiquette : a nous
        self.assertEqual(models.read_iid('12serphy20261029N7'), ('', '12'))
        self.assertEqual(expand_iid('12serphy20261029N7'), '')
        self.assertEqual(models.read_iid('M001'), ('', ''))
        self.assertEqual(models.read_id('PCsacpse00000003', models.LOT_ID_LENGTH), ('sacpse00000003', ''))
        self.assertEqual(models.read_id('sacpse00000003', models.LOT_ID_LENGTH), ('sacpse00000003', ''))
        self.assertEqual(models.read_id('12sacpse00000003', models.LOT_ID_LENGTH), ('', '12'))
        self.assertEqual(models.read_id('AB0000002B', models.PACK_ID_LENGTH), ('', 'AB'))

        # codes imprimes : items, lots (etiquettes publique et privee), paquets fermes, poste au courant
        item = self.create(self.compresses, self.today + timedelta(days=90), 1)[0]
        Items.objects.filter(iid=item.iid).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual(body['id'], self.lot.id)  # la base et l'API gardent les id sans code
        self.assertTrue(body['public_url'].endswith(f'verif?lot=PC{self.lot.id}'))
        entry = next(entry for entry in body['items'] if entry['iid'] == item.iid)
        self.assertEqual(entry['iid_short'], 'PC' + compact_iid(item.iid))
        self.assertTrue(entry['url'].endswith('item?id=PC' + compact_iid(item.iid)))
        self.assertTrue(entry['task_url'].endswith('task?id=PC' + compact_iid(item.iid)))
        self.assertIn(f'verif?lot=PC{self.lot.id}&key=', ser.lot_dict(self.lot, local=True)['private_url'])
        pack = models.SealedPacks.objects.create(item_type=self.compresses, count=3, created_by='test')
        self.assertTrue(ser.sealed_pack_dict(pack)['url'].endswith(f'pack?id=PC{pack.id}'))
        self.assertEqual(self.call('GET', '/api/health/')[1]['code_prefix'], 'PC')
        # pages web : le code est ecrit dedans (lecture des etiquettes, meme hors connexion)
        for url in ('/', '/scanner?s=x&k=y'):
            self.assertIn(b'<meta name="qrprotec-code-prefix" content="PC">', self.client.get(url).content, url)

    def test_decoder_served_with_encoder_table(self) -> None:
        """Le decodeur JS est servi aux deux pages et sa table est celle de l'encodeur du poste (code128.cpp)."""
        import re
        from pathlib import Path
        self.assertEqual(self.client.get('/web/code128.js').status_code, 200)
        web = Path(__file__).resolve().parents[1] / 'web'
        for page in ('index.html', 'scanner.html'):
            self.assertIn('web/code128.js', (web / page).read_text(encoding='utf-8'))
        js = (web / 'code128.js').read_text(encoding='utf-8')
        js_patterns = re.findall(r"'([1-4]{6,7})'", js[js.index('const PATTERNS'):js.index('];')])
        self.assertEqual(len(js_patterns), 107)
        source = Path(__file__).resolve().parents[3] / 'app' / 'src' / 'core' / 'code128.cpp'
        if not source.exists():
            self.skipTest("sources de l'app absentes")
        cpp = source.read_text(encoding='utf-8')
        cpp_patterns = re.findall(r'"([1-4]{6,7})"', cpp[cpp.index('kCode128Patterns'):cpp.index('};')])
        self.assertEqual(js_patterns, cpp_patterns)


class ScanBubblePrefTests(ApiTestCase):
    """Reglages du front web (WEB_PREFS) : « Bulle de scan » (bouton ↘ de l'ecran de scan) decochee par defaut."""

    def test_scan_bubble_default_and_change(self) -> None:
        credentials = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', '/api/auth/', credentials, local=False)
        self.assertEqual(body['web_prefs']['scan_bubble'], False)
        code, body = self.call('POST', '/api/me/prefs/', {'user': credentials, 'web_prefs': {'scan_bubble': True}},
                               local=False)
        self.assertEqual(code, 200)
        self.assertTrue(body['web_prefs']['scan_bubble'])
        self.user.refresh_from_db()
        self.assertTrue(self.user.web_pref('scan_bubble'))
        code, _ = self.call('POST', '/api/me/prefs/', {'web_prefs': {'scan_bubble': False}}, local=False)
        self.assertEqual(code, 403)
