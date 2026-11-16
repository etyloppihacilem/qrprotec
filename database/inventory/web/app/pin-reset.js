/* QRProtec - front web : deblocage d'un PIN bloque par un admin. */
'use strict';

// PIN bloque apres trop d'essais : l'utilisateur envoie a un admin la photo de l'ecran (QR code du lien de
// deblocage) ; l'admin scanne le lien, se connecte avec son badge et reinitialise le PIN. L'utilisateur
// en choisit alors un nouveau a sa prochaine connexion.

// Code oublie : le PIN est bloque comme apres trop d'essais, et l'ecran du lien de deblocage s'affiche
async function forgotPin(matricule, key) {
  if (!await confirmDialog('Code PIN oublié ?', "Votre code PIN sera bloqué jusqu'à ce que le TECH le réinitialise.", 'Bloquer mon code', 'warning')) return;
  $('#pin').close();
  try {
    await api('pin-forgot/', { matricule, key });
  } catch (e) {
    if (e.data && e.data.pin_blocked) {
      feedback.info();
      showInfo('bad', 'Code PIN oublié', e.message);
      showPinBlocked(e.data.pin_reset || {});
      return;
    }
    if (e.data && e.data.pin_setup_required) { askPin(matricule, key, pinSetupOf(e.data, matricule), ''); return; }
    feedback.bad();
    showInfo('bad', 'Code oublié', e.message);
  }
}

function showPinBlocked(reset) {
  const dialog = $('#pin-blocked');
  $('#pin-blocked-title').textContent = reset.forgotten ? 'Code PIN oublié' : 'Code PIN bloqué';
  $('#pin-blocked-text').textContent = `${reset.name || ''} : `
    + (reset.forgotten ? 'code PIN oublié. ' : 'trop de codes PIN faux. ')
    + 'Le TECH doit réinitialiser votre PIN, vous en choisirez un nouveau à la prochaine connexion.';
  $('#pin-blocked-notified').hidden = !reset.notified;
  $('#pin-blocked-contact').textContent = reset.contact ? `à ${reset.contact}` : 'au TECH';
  const link = $('#pin-blocked-link');
  link.href = reset.url || '';
  link.textContent = reset.url || '';
  const image = $('#pin-blocked-qr');
  image.hidden = !reset.url || typeof qrcode !== 'function';
  if (!image.hidden) {
    const qr = qrcode(0, 'M');
    qr.addData(reset.url);
    qr.make();
    image.src = qr.createDataURL(8, 4);
  }
  const share = $('#pin-blocked-share');
  share.hidden = !reset.url || !navigator.share;
  share.onclick = () => navigator.share({
    title: 'QRProtec : PIN bloqué',
    text: `Débloquer le code PIN de ${reset.name || reset.matricule}`,
    url: reset.url,
  }).catch(() => { /* partage annule */ });
  $('#pin-blocked-close').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}

function startPinReset(matricule, token) {
  state.pinReset = { matricule, token };
  try { sessionStorage.setItem(STORAGE_PIN_RESET, JSON.stringify(state.pinReset)); } catch (e) { /* ignore */ }
  return openPinReset();
}

function endPinReset() {
  state.pinReset = null;
  try { sessionStorage.removeItem(STORAGE_PIN_RESET); } catch (e) { /* ignore */ }
  $('#pin-reset').close();
}

async function openPinReset() {
  const request = state.pinReset;
  if (!request) return;
  if (!isAdmin()) {
    feedback.info();
    showInfo('warn', "Déblocage d'un code PIN", 'Scannez votre badge administrateur pour continuer.');
    return;
  }
  const body = { user: badge(), matricule: request.matricule, token: request.token };
  let info;
  try {
    info = await api('pin-reset/', body);
  } catch (e) {
    if (e.data && e.data.pin_required) {
      // session de l'admin expiree : il rescanne son badge, le deblocage reprend ensuite
      state.user = null;
      save();
      render();
      showInfo('warn', "Déblocage d'un code PIN", 'Session expirée : scannez à nouveau votre badge administrateur.');
      return;
    }
    endPinReset();
    feedback.bad();
    showInfo('bad', 'Déblocage impossible', e.message);
    return;
  }
  feedback.info();
  const dialog = $('#pin-reset');
  const fact = (label, value) => [el('dt', {}, label), el('dd', {}, value)];
  $('#pin-reset-body').replaceChildren(
    el('dl', { class: 'facts' },
      fact('Utilisateur', info.name),
      fact('Matricule', info.matricule),
      fact('Rôle', info.fonction || 'Secouriste'),
      fact('Droits', info.role_label),
      fact('Bloqué', info.blocked_since ? cap(fmtDateTime(info.blocked_since, { long: true })) : '-'),
      fact('Essais faux', String(info.failures)),
      info.contact ? fact('Admin à contacter', info.contact) : [],
      info.active ? [] : fact('Compte', 'désactivé')),
    el('p', { class: 'hint' }, "Vérifiez que la demande vient bien de cette personne. Après réinitialisation, elle "
      + 'choisira un nouveau PIN à sa prochaine connexion avec son badge. Si le badge a pu être perdu ou volé, '
      + 'renouvelez plutôt le badge depuis le poste.'));
  $('#pin-reset-error').textContent = '';
  const confirmButton = $('#pin-reset-confirm');
  confirmButton.disabled = false;
  confirmButton.onclick = async () => {
    confirmButton.disabled = true;
    try {
      await api('pin-reset/', { ...body, confirm: true });
    } catch (e) {
      confirmButton.disabled = false;
      $('#pin-reset-error').textContent = e.message;
      return;
    }
    endPinReset();
    feedback.good();
    showInfo('ok', 'Code PIN réinitialisé', `${info.name} choisira un nouveau PIN à sa prochaine connexion.`);
  };
  $('#pin-reset-cancel').onclick = endPinReset;
  if (!dialog.open) dialog.showModal();
}
