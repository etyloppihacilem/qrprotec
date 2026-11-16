/* QRProtec - scenarios de vérif communs, joues par le front web (planVerif de web/app/verif-plan.js).
 *
 *   node --test tests/verif-scenarios/web.test.js      (aussi lance par manage.py test : VerifScenariosTests)
 *
 * Le serveur (database/inventory/tests/test_verif_scenarios.py) et le poste (app/tests/verif_scenarios_test.cpp)
 * jouent les memes fichiers *.json : voir LISEZMOI.md.
 */
'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { planVerif } = require('../../database/inventory/web/app/verif-plan.js');

const DIR = __dirname;

// iid d'un item du scenario : type, date (perime : 2020, frais : 2099, non perissable : sans date), compteur dans
// l'ordre du fichier (le meme ordre que les iids crees par le serveur pour un meme type et une meme date)
function iids(scenario) {
  const result = {};
  let counter = 0;
  const items = [...scenario.lots.flatMap((lot) => lot.items || []), ...(scenario.elsewhere || [])];
  for (const item of items) {
    const type = scenario.types[item.type] || {};
    const date = item.expired ? '20200101' : type.perissable === false ? '00000000' : '20991231';
    result[item.id] = item.type + date + String(++counter).padStart(8, '0');
  }
  return result;
}

function play(scenario) {
  const iid = iids(scenario);
  const label = Object.fromEntries(Object.entries(iid).map(([id, value]) => [value, id]));
  const types = scenario.types;
  const lots = scenario.lots.map((lot) => ({
    id: lot.id,
    parent: lot.parent || null,
    storage: !!lot.storage,
    requirements: Object.entries(lot.requirements || {}).map(([type, required]) => ({
      type, required, check: (types[type] || {}).physical === false,
    })),
    items: (lot.items || []).map((item) => ({
      iid: iid[item.id],
      tear_off: !!(types[item.type] || {}).tear_off,
      consumable: (types[item.type] || {}).consumable !== false,
    })),
    missing_items: [],
  }));
  const expired = new Set([...scenario.lots.flatMap((lot) => lot.items || []), ...(scenario.elsewhere || [])]
    .filter((item) => item.expired).map((item) => item.id));
  const entries = scenario.scans.map((scan) => (scan.check
    ? { kind: 'check', iid: scan.lot, error: '', items: [], place: scan.lot, info: { type: scan.check } }
    : { kind: 'item', iid: iid[scan.item], error: '', expired: expired.has(scan.item), place: scan.place || '', info: {} }));
  // vérif en cascade cote front : le lot verifie a des sous-lots (inCascade)
  const cascade = scenario.lots.some((lot) => lot.parent);
  const plan = planVerif(lots, entries, { cascade });
  return {
    target: Object.fromEntries(Object.entries(plan.target).map(([value, lot]) => [label[value], lot])),
    complete: plan.lots.filter((row) => row.complete).map((row) => row.lot.id).sort(),
    discard: [...plan.discard].map((value) => label[value]).sort(),
  };
}

for (const name of fs.readdirSync(DIR).filter((file) => file.endsWith('.json')).sort()) {
  const scenario = JSON.parse(fs.readFileSync(path.join(DIR, name), 'utf8'));
  test(name, () => {
    const result = play(scenario);
    const expected = scenario.expected;
    assert.deepStrictEqual(result.target, expected.target, `${name} : lot de chaque item`);
    assert.deepStrictEqual(result.complete, [...expected.complete].sort(), `${name} : lots complets`);
    assert.deepStrictEqual(result.discard, [...(expected.discard || [])].sort(), `${name} : perimes a jeter`);
  });
}
