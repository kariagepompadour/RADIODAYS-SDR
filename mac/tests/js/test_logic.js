// Pure-logic tests of the Receiver Manager page (node mac/tests/js/test_logic.js)
const assert = require('assert');
const L = require('../../manager/app.js');
let n = 0; const t = (name, f) => { f(); n++; console.log('ok -', name); };

t('moveItem', () => { assert.deepStrictEqual(L.moveItem([1,2,3,4], 0, 2), [2,3,1,4]); assert.deepStrictEqual(L.moveItem([1,2], 0, 5), [1,2]); });
t('keyOf is case-insensitive', () => assert.strictEqual(L.keyOf({host:' A.Example.org ', port:'8073'}), 'a.example.org:8073'));
t('validateServer ok', () => assert.deepStrictEqual(L.validateServer({host:'a.example.org', port:8073, name:'A', short:'A', lat:null, lon:null}), []));
t('validateServer problems', () => {
  const p = L.validateServer({host:'http://x', port:0, name:'日本', short:'', lat:'1', lon:''});
  ['host_format','port','name_ascii','short','latlon_pair'].forEach(x => assert(p.includes(x), x));
  assert(L.validateServer({host:'a.b', port:1, name:'x', short:'x', lat:91, lon:0}).includes('latlon_range'));
});
t('validateList duplicates/empty/too many', () => {
  const s = {host:'a.b', port:1, name:'x', short:'x'};
  assert(L.validateList([s, s]).list[1].includes('duplicate'));
  assert(L.validateList([]).global.includes('empty'));
  assert(L.validateList(Array.from({length:33}, (_, i) => Object.assign({}, s, {host:'h'+i+'.b'}))).global.includes('too_many'));
});
t('uniqueShort', () => { assert.strictEqual(L.uniqueShort('Osaka JP', ['Osaka JP']), 'Osaka JP 2'); assert(L.uniqueShort('X'.repeat(20), ['X'.repeat(20)]).length <= 20); });
t('shortFromName', () => assert.strictEqual(L.shortFromName('Osaka, Japan (x)'), 'Osaka Japan'));
t('candidateToServer marks position as unverified source text', () => {
  const s = L.candidateToServer({id:'a', name:'N', short:'S', host:'h.b', port:8073, lat:1, lon:2}, [], '2026-10-24');
  assert(/Please verify/.test(s.pos_source)); assert.strictEqual(s.password, '');
});
t('toPayload password handling', () => {
  const base = {name:'A', short:'A', host:'a.b', port:'8073', lat:'', lon:''};
  assert.strictEqual(L.toPayload(base).port, 8073); assert.strictEqual(L.toPayload(base).lat, null);
  assert.strictEqual(L.toPayload(Object.assign({}, base, {password_set:true})).keep_password, true);
  assert.strictEqual(L.toPayload(Object.assign({}, base, {password_set:true, password:'p'})).password, 'p');
  assert.strictEqual(L.toPayload(Object.assign({}, base, {password_set:false, clear_password:true})).keep_password, undefined);
});
t('filterCandidates', () => {
  const c = [{name:'A Tokyo', short:'A', host:'a.b', port:1, loc:'', country:'Japan'}, {name:'B', short:'B', host:'b.b', port:2, loc:'', country:'Chile'}];
  assert.strictEqual(L.filterCandidates(c, {jponly:true}, new Set()).length, 1);
  assert.strictEqual(L.filterCandidates(c, {reg:'reg'}, new Set(['b.b:2'])).length, 1);
  assert.strictEqual(L.filterCandidates(c, {q:'tokyo'}, new Set()).length, 1);
});
const tt = (k, ...a) => k + (a.length ? ':' + a.join(',') : '');
const NOW = Date.parse('2026-10-24T05:12:33Z');          // 14:12 in Tokyo
t('describeHistory: never checked is "unchecked", for both kinds', () => {
  ['status', 'audio'].forEach(lv => { const d = L.describeHistory(lv, undefined, false, tt, 'ja', NOW, 'Asia/Tokyo'); assert.strictEqual(d.text, 'unchecked'); assert.strictEqual(d.when, ''); });
  const d = L.describeHistory('audio', { status: { ok: true, checked_at: '2026-10-24T05:00:00Z' } }, false, tt, 'ja', NOW, 'Asia/Tokyo');
  assert.strictEqual(d.text, 'unchecked');                  // an HTTP result says nothing about audio
});
t('describeHistory: HTTP and audio are described separately and never claim more', () => {
  const e = { status: { ok: true, available: true, users: 0, users_max: 4, checked_at: '2026-10-24T02:12:33Z' },
              audio: { ok: false, error: 'busy', checked_at: '2026-10-23T05:12:33Z' } };
  const h = L.describeHistory('status', e, false, tt, 'ja', NOW, 'Asia/Tokyo');
  assert(h.text.startsWith('http_ok')); assert.notStrictEqual(h.cls, 'ok');              // "answers" is never shown as the green "works"
  const a = L.describeHistory('audio', e, false, tt, 'ja', NOW, 'Asia/Tokyo');
  assert.strictEqual(a.cls, 'bad'); assert(a.text.startsWith('audio_ng'));
  assert.strictEqual(L.describeHistory('audio', { audio: { ok: true, elapsed: 1, blocks: 5, checked_at: '2026-10-24T05:00:00Z' } }, false, tt, 'ja', NOW, 'Asia/Tokyo').cls, 'ok');
  assert.strictEqual(L.describeHistory('status', e, true, tt, 'ja', NOW, 'Asia/Tokyo').cls, 'busy');
});
t('formatWhen: Japanese and English, year only when it differs', () => {
  assert.strictEqual(L.formatWhen('2026-10-24T05:12:33Z', 'ja', NOW, 'Asia/Tokyo'), '10月24日 14:12');
  assert.strictEqual(L.formatWhen('2026-10-24T05:12:33Z', 'en', NOW, 'Asia/Tokyo'), 'Oct 24, 2:12 PM');
  assert.strictEqual(L.formatWhen('2025-10-24T05:12:33Z', 'ja', NOW, 'Asia/Tokyo'), '2025年10月24日 14:12');
  assert.strictEqual(L.formatWhen('2025-10-24T05:12:33Z', 'en', NOW, 'Asia/Tokyo'), 'Oct 24, 2025, 2:12 PM');
  assert.strictEqual(L.formatWhen('2026-10-24T15:05:00Z', 'ja', NOW, 'Asia/Tokyo'), '10月25日 00:05');   // midnight is 00:05, not 24:05
});
t('ageText', () => {
  assert.strictEqual(L.ageText('2026-10-24T05:12:00Z', NOW, 'ja'), 'たった今');
  assert.strictEqual(L.ageText('2026-10-24T05:02:33Z', NOW, 'ja'), '10分前');
  assert.strictEqual(L.ageText('2026-10-24T02:12:33Z', NOW, 'en'), '3 h ago');
  assert.strictEqual(L.ageText('2026-10-20T05:12:33Z', NOW, 'ja'), '4日前');
  assert.strictEqual(L.ageText('2026-10-24T05:13:30Z', NOW, 'en'), 'just now');          // a clock slightly ahead
});
(async () => {
  const items = Array.from({ length: 30 }, (_, i) => i); const seen = []; let cancel = false;
  const r = await L.runLimited(items, 4, 0, async (i) => { seen.push(i); await new Promise(x => setTimeout(x, 2)); }, () => cancel);
  assert.strictEqual(r.peak, 4); assert.strictEqual(r.done, 30); assert.deepStrictEqual(seen.slice().sort((a, b) => a - b), items);
  const r2 = await L.runLimited(items, 4, 0, async (i) => { if (i === 5) cancel = true; await new Promise(x => setTimeout(x, 2)); }, () => cancel);
  assert(r2.cancelled && r2.started < 30 && r2.peak <= 4);
  const r3 = await L.runLimited([], 4, 0, async () => {}, () => false); assert.strictEqual(r3.done, 0);
  n++; console.log('ok - runLimited: never more than the limit at once, honours cancel, runs each item once');
  console.log(n + ' JS tests passed');
})().catch((e) => { console.error(e); process.exit(1); });
