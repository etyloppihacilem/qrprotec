/* QRProtec - contournement de la page de maintenance (qrprotec-manage maintenance on --code).
 *
 * 5 appuis rapides sur le logo ouvrent la saisie du code. Le cookie qrprotec_maintenance recoit SHA-256(sel:code) ;
 * Caddy laisse passer au back le navigateur qui presente un hachage connu (/etc/qrprotec/maintenance/bypass/), sinon
 * la page de maintenance revient. Sans sel dans la page (pas de --code), rien ne s'ouvre.
 */
'use strict';
(() => {
  const salt = document.body.dataset.salt || '';
  if (!/^[0-9a-f]{32}$/.test(salt) || !window.crypto || !crypto.subtle) return;
  const form = document.getElementById('bypass');
  const input = document.getElementById('bypass-code');
  const error = document.getElementById('bypass-error');
  const COOKIE = 'qrprotec_maintenance';
  let taps = [];

  document.getElementById('logo').addEventListener('click', () => {
    const now = Date.now();
    taps = taps.filter((at) => now - at < 3000).concat(now);
    if (taps.length >= 5) {
      taps = [];
      form.hidden = false;
      input.focus();
    }
  });

  async function hash(text) {
    const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
    return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
  }

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    const code = input.value.trim();
    if (!code) return;
    error.textContent = '';
    const token = await hash(`${salt}:${code}`);
    // Session du navigateur, 12 h au plus ; jamais envoye a un autre site ni en http://
    document.cookie = `${COOKIE}=${token}; Path=/; Max-Age=43200; SameSite=Strict; Secure`;
    let maintenance = true;
    try {
      const response = await fetch('/api/health/', { cache: 'no-store', headers: { Accept: 'application/json' } });
      let data = null;
      try { data = await response.json(); } catch (e) { /* back arrete : 502 sans JSON */ }
      maintenance = !!(data && data.maintenance);
    } catch (e) {
      error.textContent = 'Serveur injoignable.';
      return;
    }
    if (maintenance) {
      document.cookie = `${COOKIE}=; Path=/; Max-Age=0; SameSite=Strict; Secure`;
      input.value = '';
      error.textContent = 'Code incorrect.';
      return;
    }
    location.reload();
  });
})();
