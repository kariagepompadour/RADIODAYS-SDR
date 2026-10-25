/* RADIO DAYS SDR - Receiver Manager (browser side).  No libraries, no network access except to its own server.
 * Part 1 is plain logic (also tested with Node: mac/tests/js/test_logic.js); part 2 is the page. */
'use strict';

/* ======================================================================= part 1: logic */
const LIMITS = { max: 32, nameMax: 43, shortMax: 20 };

function isAscii(s) { return /^[\x20-\x7E]*$/.test(s); }

function shortFromName(name) {
  let s = String(name || '').replace(/[^\x20-\x7E]/g, '').trim();
  if (s.includes('(')) s = s.split('(')[0].trim();
  s = s.replace(/,/g, '').trim();
  return (s || 'Receiver').slice(0, LIMITS.shortMax).trimEnd();
}

function uniqueShort(short, usedShorts) {
  const used = new Set(usedShorts);
  let s = short.slice(0, LIMITS.shortMax), n = 2;
  while (used.has(s)) { s = (short.slice(0, LIMITS.shortMax - 2).trimEnd() + ' ' + n).slice(0, LIMITS.shortMax); n++; }
  return s;
}

function keyOf(s) { return String(s.host).trim().toLowerCase() + ':' + Number(s.port); }

function candidateToServer(c, registered, snapshotDate) {
  const used = registered.map(s => s.short);
  return {
    name: c.name, short: uniqueShort(c.short, used), host: c.host, port: c.port,
    lat: c.lat === null || c.lat === undefined ? null : c.lat, lon: c.lon === null || c.lon === undefined ? null : c.lon,
    pos_source: c.lat === null || c.lat === undefined ? '' : 'KiwiSDR.com public directory' + (snapshotDate ? ' (' + snapshotDate + ')' : '') + ': GNSS position reported by the receiver. Please verify.',
    id: c.id, password: '', password_set: false, keep_password: false,
  };
}

function moveItem(arr, from, to) {
  const a = arr.slice();
  if (from < 0 || from >= a.length || to < 0 || to >= a.length || from === to) return a;
  const [x] = a.splice(from, 1);
  a.splice(to, 0, x);
  return a;
}

/* One entry.  Returns a list of problems (empty = fine); the same rules as the program applies when it saves. */
function validateServer(s) {
  const p = [];
  const host = String(s.host || '').trim();
  if (!host) p.push('host');
  else if (/[:\/ ]/.test(host) || !/^[A-Za-z0-9]([A-Za-z0-9_-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9_-]{0,61}[A-Za-z0-9])?)*$/.test(host)) p.push('host_format');
  const port = Number(s.port);
  if (!Number.isInteger(port) || port < 1 || port > 65535) p.push('port');
  const name = String(s.name || '').trim();
  if (!name) p.push('name'); else if (name.length > LIMITS.nameMax) p.push('name_long'); else if (!isAscii(name)) p.push('name_ascii');
  const short = String(s.short || '').trim();
  if (!short) p.push('short'); else if (short.length > LIMITS.shortMax) p.push('short_long'); else if (!isAscii(short)) p.push('short_ascii');
  const hasLat = !(s.lat === null || s.lat === undefined || s.lat === ''), hasLon = !(s.lon === null || s.lon === undefined || s.lon === '');
  if (hasLat !== hasLon) p.push('latlon_pair');
  else if (hasLat) {
    const la = Number(s.lat), lo = Number(s.lon);
    if (!isFinite(la) || !isFinite(lo) || la < -90 || la > 90 || lo < -180 || lo > 180) p.push('latlon_range');
  }
  return p;
}

function validateList(list) {
  const out = { list: [], global: [] };
  const seen = new Map();
  list.forEach((s, i) => {
    const problems = validateServer(s);
    const k = keyOf(s);
    if (seen.has(k)) problems.push('duplicate');
    else seen.set(k, i);
    out.list.push(problems);
  });
  if (list.length === 0) out.global.push('empty');
  if (list.length > LIMITS.max) out.global.push('too_many');
  return out;
}

function listsEqual(a, b) { return JSON.stringify(a) === JSON.stringify(b); }

/* what goes to the program: numbers as numbers, empty position as null, password only when typed */
function toPayload(s) {
  const hasPos = !(s.lat === null || s.lat === undefined || s.lat === '') && !(s.lon === null || s.lon === undefined || s.lon === '');
  const o = {
    name: String(s.name).trim(), short: String(s.short).trim(), host: String(s.host).trim(), port: Number(s.port),
    lat: hasPos ? Number(s.lat) : null, lon: hasPos ? Number(s.lon) : null,
  };
  if (s.pos_source) o.pos_source = s.pos_source;
  if (s.note) o.note = s.note;
  if (s.name_ja) o.name_ja = s.name_ja;
  if (s.id) o.id = s.id;
  if (s.password) o.password = s.password;
  else if (s.password_set && !s.clear_password) o.keep_password = true;
  return o;
}

function filterCandidates(cands, f, registeredKeys) {
  const q = (f.q || '').trim().toLowerCase();
  return cands.filter(c => {
    if (f.jponly && c.country !== 'Japan') return false;
    if (f.country && f.country !== 'all' && c.country !== f.country) return false;
    const reg = registeredKeys.has(keyOf(c));
    if (f.reg === 'reg' && !reg) return false;
    if (f.reg === 'unreg' && reg) return false;
    if (q && !(c.name + ' ' + c.short + ' ' + c.host + ' ' + c.loc + ' ' + c.country).toLowerCase().includes(q)) return false;
    return true;
  });
}

/* ---- check history: what the last check of a receiver said, and when.
 * A result is only ever "what was seen at that moment"; the wording never turns it into "works now". */
const CHECK_LEVELS = ['status', 'audio'];

/* "10月24日 14:12" / "Oct 24, 2:12 PM" (24-hour clock for Japanese); the year is added when it is not the current year.
 * tz is only for tests; the page uses the browser's own time zone (the same Mac). */
function formatWhen(iso, lang, nowMs, tz) {
  const d = new Date(iso);
  if (isNaN(d.getTime())) return '?';
  const base = tz ? { timeZone: tz } : {};
  const yearOf = (x) => Number(new Intl.DateTimeFormat('en', Object.assign({ year: 'numeric' }, base)).format(x));
  const withYear = yearOf(d) !== yearOf(new Date(nowMs));
  if (lang !== 'ja') {
    return new Intl.DateTimeFormat('en-US', Object.assign({ month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit', hour12: true }, withYear ? { year: 'numeric' } : {}, base)).format(d);
  }
  const p = {};          // "10月24日 14:12": written out, not the ambiguous 10/24
  new Intl.DateTimeFormat('en-US', Object.assign({ year: 'numeric', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' }, base))
    .formatToParts(d).forEach(x => { p[x.type] = x.value; });
  return (withYear ? p.year + '年' : '') + Number(p.month) + '月' + Number(p.day) + '日 ' + p.hour + ':' + p.minute;
}

function ageText(iso, nowMs, lang) {
  const sec = Math.round((nowMs - new Date(iso).getTime()) / 1000);
  if (!isFinite(sec)) return '';
  const ja = lang === 'ja';
  if (sec < 60) return ja ? 'たった今' : 'just now';           // also covers a clock a little ahead
  const m = Math.floor(sec / 60);
  if (m < 60) return ja ? m + '分前' : m + ' min ago';
  const h = Math.floor(m / 60);
  if (h < 48) return ja ? h + '時間前' : h + ' h ago';
  const dd = Math.floor(h / 24);
  return ja ? dd + '日前' : dd + ' days ago';
}

/* the text of one stored result (no time in it): it says exactly what was checked and nothing more */
function describeResult(r, t) {
  if (r.level === 'audio' || r.blocks !== undefined || r.in_use) {
    if (r.ok && r.in_use) return { cls: 'ok', text: t('audio_inuse') };
    if (r.ok) return { cls: 'ok', text: t('audio_ok', r.elapsed, r.blocks) };
    return { cls: 'bad', text: t('audio_ng', r.error || '?') };
  }
  if (!r.ok) return { cls: 'bad', text: t('http_ng', r.error || '?') };
  const users = r.users_max ? ' users ' + (r.users === null || r.users === undefined ? '?' : r.users) + '/' + r.users_max : '';
  if (r.available) return { cls: 'warn', text: t('http_ok', users) };
  return { cls: 'warn', text: t('http_offline', r.status || '?', r.offline || '?') };
}

/* one line of the display: level = 'status' | 'audio'; entry = history[key] (may be undefined); busy = a check is running now */
function describeHistory(level, entry, busy, t, lang, nowMs, tz) {
  if (busy) return { cls: 'busy', text: t('checking'), when: '' };
  const r = entry && entry[level];
  if (!r) return { cls: 'none', text: t('unchecked'), when: '' };
  const d = describeResult(Object.assign({ level }, r), t);
  d.when = t('last') + ' ' + formatWhen(r.checked_at, lang, nowMs, tz) + '（' + ageText(r.checked_at, nowMs, lang) + '）';
  if (lang !== 'ja') d.when = t('last') + ' ' + formatWhen(r.checked_at, lang, nowMs, tz) + ' (' + ageText(r.checked_at, nowMs, lang) + ')';
  return d;
}

/* bulk run: runs fn(item) for every item with at most `limit` in flight, starting one every `gapMs`; stops when cancel() is true */
async function runLimited(items, limit, gapMs, fn, cancel, onProgress, sleep) {
  const wait = sleep || ((ms) => new Promise(r => setTimeout(r, ms)));
  let next = 0, done = 0, peak = 0, active = 0;
  async function worker() {
    while (next < items.length && !cancel()) {
      const item = items[next++];
      active++; peak = Math.max(peak, active);
      try { await fn(item); } finally { active--; done++; if (onProgress) onProgress(done, items.length); }
      if (gapMs && next < items.length && !cancel()) await wait(gapMs);
    }
  }
  await Promise.all(Array.from({ length: Math.max(1, Math.min(limit, items.length)) }, worker));
  return { started: next, done, peak, cancelled: next < items.length };
}

if (typeof module !== 'undefined') {
  module.exports = { LIMITS, isAscii, shortFromName, uniqueShort, keyOf, candidateToServer, moveItem, validateServer, validateList,
                     listsEqual, toPayload, filterCandidates, formatWhen, ageText, describeResult, describeHistory, runLimited, CHECK_LEVELS };
}

/* ======================================================================= part 2: the page */
if (typeof document !== 'undefined') (function () {
  const TX = {
    ja: {
      intro: 'KiwiSDR 受信機を選んで並べ替え、保存すると Cardputer ADV の地球儀（G キー）の一覧に反映されます。この画面はこの PC の中だけで動いています。',
      unsaved: '未保存の変更', save: '保存して反映', discard: '変更を破棄', restore: '直前の保存に戻す',
      registered: '登録済みの受信機', registered_help: 'この順番が Cardputer の地球儀の一覧の順番です（最大 32 台）。≡ をドラッグするか ↑↓ で並べ替え、× で登録解除します。',
      manual_add: '手動で追加（ホスト名・ポート番号を直接指定）', f_host: 'ホスト名', f_port: 'ポート', f_name: '名前（ASCII 43文字まで）', f_short: '短い名前（20文字まで）',
      f_lat: '緯度（任意）', f_lon: '経度（任意）', add: '追加',
      candidates: '候補（公開 KiwiSDR 受信機）', search: '名前・場所・ホストで検索', japan_only: '日本のみ', rf_all: 'すべて', rf_reg: '登録済み', rf_unreg: '未登録',
      c_name: '受信機', c_country: '国', c_addr: 'ホスト:ポート', c_pos: '位置', c_snr: 'SNR(参考)', c_check: '確認',
      footer: '「HTTP確認」は受信機の Web サーバーが /status に答えるかだけを調べます（音声が受信できるかは分かりません）。「音声確認」は実際に約 7 秒だけ接続して音声ブロックが届くか調べます（相手の受信機の枠を一時的に 1 つ使うので、必要なときだけ押してください）。公開受信機は時間制限・人数制限・パスワードがあることがあり、稼働状況は常に変わります。各受信機の運営者のルールを守ってください。',
      guide: '操作ガイド', last: '前回', lv_status: 'HTTP', lv_audio: '音声',
      history_note: '確認結果は「前回確認した時点」のものです。「応答あり」「音声受信を確認」は、いま接続できることを保証しません。結果は日時とともに保存され、画面を閉じても残ります。画面を開いただけでは問い合わせません。',
      bulk_reg: '登録済みをHTTP再確認', bulk_cand: '表示中の候補をHTTP一括確認', bulk_stop: '中止',
      bulk_confirm: (n, c) => 'いま表示されている ' + n + ' 台の /status に HTTP で 1 回ずつ問い合わせます（同時に最大 ' + c + ' 台、少し間隔をあけて順番に）。音声は確認しません。続けますか？',
      bulk_progress: (d, n) => 'HTTP確認中 ' + d + ' / ' + n, bulk_done: (d, n) => 'HTTP確認が終わりました（' + d + ' / ' + n + ' 台）。', bulk_cancelled: (d, n) => 'HTTP確認を中止しました（' + d + ' / ' + n + ' 台まで）。', bulk_none: '確認する受信機がありません。',
      not_run: '確認を実行できませんでした: ',
      unchecked: '未確認', checking: '確認中…', audio_inuse: '受信中だったため、実際の受信で確認', audio_ok: (s, b) => '音声受信を確認（' + s + '秒、' + b + 'ブロック）',
      audio_ng: e => '音声を受信できず: ' + e, http_ng: e => '応答なし: ' + e, http_ok: u => '応答あり・稼働表示' + u + '（Web サーバーの応答のみ）',
      http_offline: (s, o) => '応答あり・status=' + s + ' offline=' + o + '（Web サーバーの応答のみ）',
      b_http: 'HTTP確認', b_audio: '音声確認(約7秒)', details: '詳細', pw: 'パスワード（必要な受信機のみ）', pw_set: 'パスワード設定済み（変更するときだけ入力）', pw_clear: 'パスワードを削除',
      note: 'メモ', pos_source: '位置の根拠', port_assumed: 'ポート仮定', pos_unknown: '位置未確認', remove: '登録解除',
      snapshot: n => '公開一覧（kiwisdr.com/public）のスナップショット: ' + n + '。ここにある受信機が今接続できるかは分かりません。',
      reg_n: n => n + ' / 32 台', cand_n: (a, b) => a + ' / ' + b + ' 件',
      saved: n => '保存しました（' + n + '台）。', reloaded: 'Mac で動作中のプログラムにも反映しました。', not_running: 'RADIO DAYS SDR は動いていないため、次回の起動から使われます。',
      glance: 'Cardputer では G キーで地球儀を開き直すと新しい一覧になります。', discarded: '変更を破棄しました。', restored: '直前の保存に戻しました。',
      confirm_restore: '直前に保存した内容に戻します。いまの内容は新しい「直前の保存」として残るので、もう一度押すと元に戻せます。続けますか？',
      confirm_discard: '未保存の変更を捨てます。よろしいですか？', confirm_overwrite: 'このファイルは、この画面を開いた後に別の場所で変更されています。上書き保存しますか？（キャンセルすると何も保存しません）',
      full: '登録できるのは 32 台までです。', err_prefix: '保存できません: ', fix: '赤い枠の項目を直してください。',
      v_host: 'ホスト名が必要です', v_host_format: 'ホスト名の形式が正しくありません（http:// やポート、パスは付けません）', v_port: 'ポートは 1〜65535', v_name: '名前が必要です',
      v_name_long: '名前は 43 文字まで', v_name_ascii: '名前は ASCII のみ（Cardputer が表示できません）', v_short: '短い名前が必要です', v_short_long: '短い名前は 20 文字まで',
      v_short_ascii: '短い名前は ASCII のみ', v_latlon_pair: '緯度と経度は両方入れるか、両方空に', v_latlon_range: '緯度は -90〜90、経度は -180〜180', v_duplicate: '同じホスト:ポートが重複しています',
      v_empty: '1 台以上登録してください', v_too_many: '登録は 32 台までです',
      server_down: 'Mac のプログラムに接続できません。', running: 'RADIO DAYS SDR 動作中', standalone: '単独起動（RADIO DAYS SDR は未起動）', now: '受信中',
      cleared_in_use: 'いま受信中の受信機を外すと、保存後に先頭の受信機へ切り替わります。', up: '上へ', down: '下へ',
    },
    en: {
      intro: 'Choose and order KiwiSDR receivers; after saving, the list appears in the Cardputer ADV globe (G key). This page only runs on this computer.',
      unsaved: 'unsaved changes', save: 'Save and apply', discard: 'Discard changes', restore: 'Restore previous save',
      registered: 'Registered receivers', registered_help: 'This order is the order of the Cardputer globe list (up to 32). Drag ≡ or use ↑↓ to reorder, × to unregister.',
      manual_add: 'Add by hand (host name and port)', f_host: 'Host', f_port: 'Port', f_name: 'Name (ASCII, max 43)', f_short: 'Short name (max 20)',
      f_lat: 'Latitude (optional)', f_lon: 'Longitude (optional)', add: 'Add',
      candidates: 'Candidates (public KiwiSDR receivers)', search: 'Search name, place, host', japan_only: 'Japan only', rf_all: 'All', rf_reg: 'Registered', rf_unreg: 'Not registered',
      c_name: 'Receiver', c_country: 'Country', c_addr: 'Host:port', c_pos: 'Position', c_snr: 'SNR (ref.)', c_check: 'Check',
      footer: '"HTTP check" only asks whether the receiver\'s web server answers /status (it does not show that audio can be received). "Audio check" makes one real connection for about 7 seconds and shows whether audio blocks arrive (it uses one user slot of that receiver briefly, so press it only when needed). Public receivers may have time limits, user limits or passwords, and their status changes all the time. Please respect each receiver owner\'s rules.',
      guide: 'Operation Guide', last: 'Last', lv_status: 'HTTP', lv_audio: 'Audio',
      history_note: 'Results are from the time of the last check. "Answers" and "audio received" do not guarantee a connection now. Results are saved with their date and time and stay after the page is closed. Opening the page asks nothing.',
      bulk_reg: 'Re-check registered (HTTP)', bulk_cand: 'Check shown candidates (HTTP)', bulk_stop: 'Stop',
      bulk_confirm: (n, c) => 'This asks /status of the ' + n + ' receivers now shown, one HTTP request each (at most ' + c + ' at a time, with short gaps). Audio is not checked. Continue?',
      bulk_progress: (d, n) => 'HTTP check ' + d + ' / ' + n, bulk_done: (d, n) => 'HTTP check finished (' + d + ' / ' + n + ').', bulk_cancelled: (d, n) => 'HTTP check stopped (' + d + ' / ' + n + ' done).', bulk_none: 'No receivers to check.',
      not_run: 'The check could not be run: ',
      unchecked: 'not checked', checking: 'checking…', audio_inuse: 'it was being listened to, confirmed by actual reception', audio_ok: (s, b) => 'audio received (' + s + ' s, ' + b + ' blocks)',
      audio_ng: e => 'no audio: ' + e, http_ng: e => 'failed: ' + e, http_ok: u => 'answers, reports active' + u + ' (web server answer only)',
      http_offline: (s, o) => 'answers, status=' + s + ' offline=' + o + ' (web server answer only)',
      b_http: 'HTTP check', b_audio: 'Audio check (~7 s)', details: 'details', pw: 'Password (only if the receiver needs one)', pw_set: 'password is set (type only to change)', pw_clear: 'remove password',
      note: 'Note', pos_source: 'Position source', port_assumed: 'port assumed', pos_unknown: 'position unverified', remove: 'Unregister',
      snapshot: n => 'Snapshot of the public directory (kiwisdr.com/public): ' + n + '. Whether these receivers can be reached now is unknown.',
      reg_n: n => n + ' / 32', cand_n: (a, b) => a + ' / ' + b,
      saved: n => 'Saved (' + n + ' receivers).', reloaded: 'It is loaded into the running program too.', not_running: 'RADIO DAYS SDR is not running; the list is used from its next start.',
      glance: 'On the Cardputer, open the globe (G) again to see the new list.', discarded: 'Changes discarded.', restored: 'Previous save restored.',
      confirm_restore: 'Go back to the previous save. The current list becomes the new "previous save", so pressing it again undoes this. Continue?',
      confirm_discard: 'Discard the unsaved changes?', confirm_overwrite: 'This file was changed elsewhere after you opened this page. Overwrite it? (Cancel saves nothing.)',
      full: 'At most 32 receivers can be registered.', err_prefix: 'Cannot save: ', fix: 'Please fix the items with a red frame.',
      v_host: 'host is required', v_host_format: 'not a valid host name (no http://, port or path)', v_port: 'port must be 1-65535', v_name: 'name is required',
      v_name_long: 'name: 43 characters at most', v_name_ascii: 'name must be ASCII (the Cardputer cannot show anything else)', v_short: 'short name is required', v_short_long: 'short name: 20 characters at most',
      v_short_ascii: 'short name must be ASCII', v_latlon_pair: 'give both latitude and longitude, or neither', v_latlon_range: 'latitude -90..90, longitude -180..180', v_duplicate: 'the same host:port is listed twice',
      v_empty: 'register at least one receiver', v_too_many: 'at most 32 receivers',
      server_down: 'Cannot reach the Mac program.', running: 'RADIO DAYS SDR is running', standalone: 'stand-alone (RADIO DAYS SDR is not running)', now: 'in use',
      cleared_in_use: 'If you remove the receiver in use, the first receiver is connected after saving.', up: 'up', down: 'down',
    },
  };
  let lang = (function () { try { const l = localStorage.getItem('rdsdr-lang'); if (l === 'ja' || l === 'en') return l; } catch (e) { /* no storage */ } return 'en'; })();   // English unless the user chose Japanese (the choice is remembered)
  const t = (k, ...a) => { const v = TX[lang][k]; return typeof v === 'function' ? v(...a) : (v === undefined ? k : v); };

  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  const state = { servers: [], baseline: [], file: null, candidates: [], snapshot: '', current: null, running: false, history: {}, busy: new Set(), conc: 4, bulk: { running: false, cancel: false }, expanded: new Set(), errors: null, dragFrom: -1 };

  function toast(msg, bad) {
    const x = $('toast'); x.textContent = msg; x.className = 'toast' + (bad ? ' bad' : ''); x.hidden = false;
    clearTimeout(toast.t); toast.t = setTimeout(() => { x.hidden = true; }, bad ? 9000 : 6500);
  }
  async function api(path, body) {
    const opt = body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-RDSDR-Manager': '1' }, body: JSON.stringify(body) };
    const r = await fetch(path, opt);
    let j = {}; try { j = await r.json(); } catch (e) { /* not JSON */ }
    if (!r.ok) { const err = new Error(j.error || ('HTTP ' + r.status)); err.status = r.status; err.code = j.code; throw err; }
    return j;
  }
  const payloadList = () => state.servers.map(toPayload);
  const isDirty = () => !listsEqual(payloadList(), state.baseline.map(toPayload));
  const regKeys = () => new Set(state.servers.map(keyOf));

  /* ---------------- rendering */
  function applyStatic() {
    document.documentElement.lang = lang;
    document.querySelectorAll('[data-i18n]').forEach(n => { n.textContent = t(n.dataset.i18n); });
    document.querySelectorAll('[data-i18n-placeholder]').forEach(n => { n.placeholder = t(n.dataset.i18nPlaceholder); });
    $('candnote').textContent = t('snapshot', state.snapshot || '?');
  }
  function renderBar() {
    $('dirty').hidden = !isDirty();
    $('filename').textContent = state.file ? state.file.file : 'servers.json';
    $('restore').disabled = !(state.file && state.file.backup_exists);
    $('runinfo').textContent = ' · ' + (state.running ? t('running') : t('standalone'));
  }
  function problemText(code) { return t('v_' + code); }

  const histKey = (key, level) => key + '|' + level;
  /* two lines: the last HTTP check and the last audio check of this receiver (each "not checked" if there is none) */
  function fillChips(box) {
    const key = box.dataset.key, entry = state.history[key], now = Date.now();
    box.textContent = '';
    CHECK_LEVELS.forEach(level => {
      const d = describeHistory(level, entry, state.busy.has(histKey(key, level)), t, lang, now);
      const row = el('div', 'chiprow');
      row.append(el('span', 'lv', t('lv_' + level)), el('span', 'chip ' + d.cls, d.text));
      if (d.when) row.append(el('span', 'when muted', d.when));
      box.append(row);
    });
  }
  function checkChips(key) { const b = el('div', 'chips'); b.dataset.key = key; fillChips(b); return b; }
  function refreshChips(key) { document.querySelectorAll('.chips').forEach(b => { if (!key || b.dataset.key === key) fillChips(b); }); }

  function field(cls, value, onInput, attrs) {
    const i = el('input', cls); i.value = value === null || value === undefined ? '' : value;
    Object.entries(attrs || {}).forEach(([k, v]) => i.setAttribute(k, v));
    i.addEventListener('input', () => onInput(i.value));
    return i;
  }

  function renderRegistered() {
    const box = $('registered'); box.textContent = '';
    const v = state.errors || validateList(state.servers);
    state.servers.forEach((s, i) => {
      const row = el('div', 'row' + ((v.list[i] || []).length && state.errors ? ' invalid' : ''));
      const l1 = el('div', 'line');
      const h = el('span', 'handle', '≡'); h.draggable = true; h.title = '↕';
      h.addEventListener('dragstart', (e) => { state.dragFrom = i; e.dataTransfer.effectAllowed = 'move'; try { e.dataTransfer.setData('text/plain', String(i)); } catch (x) { /* ok */ } });
      row.addEventListener('dragover', (e) => { if (state.dragFrom >= 0) { e.preventDefault(); row.classList.add('dragover'); } });
      row.addEventListener('dragleave', () => row.classList.remove('dragover'));
      row.addEventListener('drop', (e) => { e.preventDefault(); row.classList.remove('dragover'); if (state.dragFrom >= 0) { state.servers = moveItem(state.servers, state.dragFrom, i); state.dragFrom = -1; refresh(); } });
      h.addEventListener('dragend', () => { state.dragFrom = -1; });
      l1.append(h, el('span', 'idx', String(i + 1)));
      l1.append(field('in-short', s.short, (x) => { s.short = x; touch(); }, { maxlength: 20, 'aria-label': 'short' }));
      l1.append(field('in-name', s.name, (x) => { s.name = x; touch(); }, { maxlength: 43, 'aria-label': 'name' }));
      const up = el('button', 'small', '↑'); up.title = t('up'); up.disabled = i === 0; up.type = 'button';
      up.addEventListener('click', () => { state.servers = moveItem(state.servers, i, i - 1); refresh(); });
      const dn = el('button', 'small', '↓'); dn.title = t('down'); dn.disabled = i === state.servers.length - 1; dn.type = 'button';
      dn.addEventListener('click', () => { state.servers = moveItem(state.servers, i, i + 1); refresh(); });
      const rm = el('button', 'small', '×'); rm.title = t('remove'); rm.type = 'button';
      rm.addEventListener('click', () => { state.servers.splice(i, 1); refresh(); });
      l1.append(up, dn, rm);
      row.append(l1);
      const l2 = el('div', 'line');
      l2.append(field('in-host mono', s.host, (x) => { s.host = x; touch(); }, { 'aria-label': 'host', spellcheck: 'false' }));
      l2.append(field('in-port', s.port, (x) => { s.port = x; touch(); }, { type: 'number', min: 1, max: 65535, 'aria-label': 'port' }));
      l2.append(field('in-lat', s.lat, (x) => { s.lat = x; touch(); }, { placeholder: 'lat', 'aria-label': 'lat' }));
      l2.append(field('in-lon', s.lon, (x) => { s.lon = x; touch(); }, { placeholder: 'lon', 'aria-label': 'lon' }));
      row.append(l2);
      const key = keyOf(s);
      const l3 = el('div', 'line');
      l3.append(checkChips(key));
      const b1 = el('button', 'small', t('b_http')); b1.type = 'button'; b1.addEventListener('click', () => runCheck(s.host, s.port, 'status'));
      const b2 = el('button', 'small', t('b_audio')); b2.type = 'button'; b2.addEventListener('click', () => runCheck(s.host, s.port, 'audio'));
      l3.append(b1, b2);
      if (state.current && keyOf(state.current) === key) l3.append(el('span', 'chip ok', t('now') + ' · ' + state.current.link));
      if (s.lat === null || s.lat === '' || s.lat === undefined) l3.append(el('span', 'tag', t('pos_unknown')));
      const dt = el('button', 'small ghost', t('details') + (state.expanded.has(key) ? ' ▲' : ' ▼')); dt.type = 'button';
      dt.addEventListener('click', () => { state.expanded.has(key) ? state.expanded.delete(key) : state.expanded.add(key); renderRegistered(); });
      l3.append(dt);
      row.append(l3);
      if (state.expanded.has(key)) {
        const d = el('div', 'details');
        const lp = el('label', 'l'); lp.append(el('span', '', t('pw') + ': '));
        const pw = el('input', ''); pw.type = 'password'; pw.autocomplete = 'new-password'; pw.value = s.password || '';
        pw.placeholder = s.password_set ? t('pw_set') : ''; pw.addEventListener('input', () => { s.password = pw.value; touch(); });
        lp.append(pw);
        if (s.password_set) { const c = el('button', 'small', t('pw_clear')); c.type = 'button'; c.addEventListener('click', () => { s.password_set = false; s.password = ''; s.clear_password = true; touch(); renderRegistered(); }); lp.append(c); }
        d.append(lp);
        const ln = el('label', 'l'); ln.append(el('span', '', t('pos_source') + ': '));
        ln.append(field('in-name', s.pos_source || '', (x) => { s.pos_source = x; touch(); }, { maxlength: 300 }));
        d.append(ln);
        row.append(d);
      }
      (v.list[i] || []).forEach(p => row.append(el('p', 'err', problemText(p))));
      box.append(row);
    });
    $('regcount').textContent = t('reg_n', state.servers.length);
    (v.global || []).forEach(p => box.append(el('p', 'err', problemText(p))));
    if (state.current && !state.servers.some(s => keyOf(s) === keyOf(state.current)) && state.running) box.append(el('p', 'err', t('cleared_in_use')));
  }

  function renderCandidates() {
    const tb = document.querySelector('#cands tbody'); tb.textContent = '';
    const keys = regKeys();
    const f = { q: $('q').value, country: $('country').value, jponly: $('jponly').checked, reg: $('regfilter').value };
    const list = filterCandidates(state.candidates, f, keys);
    list.forEach(c => {
      const key = keyOf(c);
      const tr = el('tr', keys.has(key) ? 'isreg' : '');
      const td0 = el('td'); const cb = el('input'); cb.type = 'checkbox'; cb.checked = keys.has(key); cb.setAttribute('aria-label', c.short);
      cb.addEventListener('change', () => toggleCandidate(c, cb.checked, cb)); td0.append(cb);
      const td1 = el('td', 'c-name'); td1.append(el('div', 'nm', c.short), el('div', 'loc', c.name));
      const td2 = el('td', '', c.country);
      const td3 = el('td', 'mono', c.host + ':' + c.port); if (c.port_assumed) td3.append(el('span', 'tag', t('port_assumed')));
      const td4 = el('td', '', c.lat === null || c.lat === undefined ? '—' : c.lat.toFixed(2) + ', ' + c.lon.toFixed(2));
      const td5 = el('td', '', c.snr ? String(c.snr) : '—');
      const td6 = el('td'); td6.append(checkChips(key));
      const b = el('button', 'small', t('b_http')); b.type = 'button'; b.addEventListener('click', () => runCheck(c.host, c.port, 'status'));
      const b2 = el('button', 'small', t('b_audio')); b2.type = 'button'; b2.addEventListener('click', () => runCheck(c.host, c.port, 'audio')); td6.append(b, ' ', b2);
      tr.append(td0, td1, td2, td3, td4, td5, td6); tb.append(tr);
    });
    $('candcount').textContent = t('cand_n', list.length, state.candidates.length);
  }

  function fillCountries() {
    const sel = $('country'); const cur = sel.value || 'all'; sel.textContent = '';
    const o = el('option', '', '—'); o.value = 'all'; sel.append(o);
    [...new Set(state.candidates.map(c => c.country))].sort((a, b) => (a === 'Japan' ? -1 : b === 'Japan' ? 1 : a.localeCompare(b))).forEach(c => { const op = el('option', '', c); op.value = c; sel.append(op); });
    sel.value = cur;
  }

  function touch() { renderBar(); }                                  // typing: only the bar (keeps the cursor in the field)
  function refresh() { state.errors = null; renderBar(); renderRegistered(); renderCandidates(); }

  /* ---------------- actions */
  function toggleCandidate(c, on, cb) {
    const key = keyOf(c);
    if (on) {
      if (state.servers.length >= LIMITS.max) { cb.checked = false; toast(t('full'), true); return; }
      if (!state.servers.some(s => keyOf(s) === key)) state.servers.push(candidateToServer(c, state.servers, (state.snapshotDate || '')));
    } else {
      state.servers = state.servers.filter(s => keyOf(s) !== key);
    }
    refresh();
  }

  /* one check.  The server stores the result (check_history.json); here it is only shown.  Returns true if a result came back. */
  async function runCheck(host, port, level) {
    const key = keyOf({ host, port }), hk = histKey(key, level);
    if (state.busy.has(hk)) return false;
    state.busy.add(hk); refreshChips(key);
    try {
      const r = await api('/api/check', { host, port: Number(port), level });
      if (r.recorded === undefined) { toast(t('not_run') + (r.error || '?'), true); return false; }   // could not be run: nothing to remember
      (state.history[key] = state.history[key] || {})[level] = r;
      return true;
    } catch (e) { toast(t('not_run') + e.message, true); return false; }
    finally { state.busy.delete(hk); refreshChips(key); }
  }

  /* HTTP check of many receivers, only when asked: confirmed first, at most state.conc at once, one new request every 150 ms at most, can be stopped */
  async function bulkCheck(list) {
    if (state.bulk.running) return;
    const seen = new Set(), items = list.filter(x => { const k = keyOf(x); if (seen.has(k)) return false; seen.add(k); return true; });
    if (!items.length) { toast(t('bulk_none'), true); return; }
    if (!confirm(t('bulk_confirm', items.length, state.conc))) return;
    state.bulk.running = true; state.bulk.cancel = false;
    ['bulkreg', 'bulkcand'].forEach(id => { $(id).disabled = true; });
    $('bulkbar').hidden = false; $('bulkprog').max = items.length; $('bulkprog').value = 0; $('bulktext').textContent = t('bulk_progress', 0, items.length);
    const r = await runLimited(items, state.conc, 150, (x) => runCheck(x.host, x.port, 'status'), () => state.bulk.cancel,
      (d, n) => { $('bulkprog').value = d; $('bulktext').textContent = t('bulk_progress', d, n); });
    state.bulk.running = false;
    ['bulkreg', 'bulkcand'].forEach(id => { $(id).disabled = false; });
    $('bulkbar').hidden = true; refreshChips();
    toast(r.cancelled ? t('bulk_cancelled', r.done, items.length) : t('bulk_done', r.done, items.length));
  }
  const visibleCandidates = () => filterCandidates(state.candidates, { q: $('q').value, country: $('country').value, jponly: $('jponly').checked, reg: $('regfilter').value }, regKeys());

  /* keep the newer result of each kind (another tab, or a check that finished while loading) */
  function mergeHistory(h) {
    Object.entries(h || {}).forEach(([k, e]) => CHECK_LEVELS.forEach(lv => {
      const n = e && e[lv]; if (!n) return;
      const cur = state.history[k] && state.history[k][lv];
      if (!cur || String(n.checked_at) > String(cur.checked_at)) (state.history[k] = state.history[k] || {})[lv] = n;
    }));
  }
  async function loadHistory() { const h = await api('/api/check-history'); state.conc = h.bulk_concurrency || state.conc; mergeHistory(h.entries); }

  async function load(initial) {
    const st = await api('/api/state');
    state.servers = st.servers.map(s => Object.assign({ password: '', password_set: !!s.password_set }, s));
    state.baseline = JSON.parse(JSON.stringify(state.servers));
    state.file = st.file; state.running = st.running; state.current = st.current || null;
    LIMITS.max = st.limits.max; LIMITS.nameMax = st.limits.name_max; LIMITS.shortMax = st.limits.short_max;
    if (initial) {
      const c = await api('/api/candidates');
      state.candidates = c.receivers || []; state.snapshot = c.directory_timestamp || ''; state.snapshotDate = (c.built || '').slice(0, 10);
      fillCountries();
      try { await loadHistory(); } catch (e) { /* the history is a convenience */ }
    }
    applyStatic(); refresh();
  }

  async function save(force) {
    const v = validateList(state.servers);
    if (v.global.length || v.list.some(p => p.length)) {
      state.errors = v; renderRegistered(); toast(t('fix'), true); return;
    }
    try {
      const r = await api('/api/save', { servers: payloadList(), base_mtime: state.file ? state.file.mtime : null, force: !!force });
      let msg = t('saved', r.count) + ' ' + (r.reloaded ? t('reloaded') : t('not_running')) + '\n' + t('glance');
      if (r.warnings && r.warnings.length) msg += '\n! ' + r.warnings.join('\n! ');
      await load(false); toast(msg);
    } catch (e) {
      if (e.status === 409 && confirm(t('confirm_overwrite'))) return save(true);
      toast(t('err_prefix') + e.message, true);
    }
  }

  function wire() {
    $('save').addEventListener('click', () => save(false));
    $('discard').addEventListener('click', async () => { if (!isDirty() || confirm(t('confirm_discard'))) { await load(false); toast(t('discarded')); } });
    $('restore').addEventListener('click', async () => {
      if (!confirm(t('confirm_restore'))) return;
      try { await api('/api/restore', {}); await load(false); toast(t('restored')); } catch (e) { toast(t('err_prefix') + e.message, true); }
    });
    $('bulkreg').addEventListener('click', () => bulkCheck(state.servers.map(x => ({ host: String(x.host).trim(), port: Number(x.port) }))));
    $('bulkcand').addEventListener('click', () => bulkCheck(visibleCandidates()));
    $('bulkstop').addEventListener('click', () => { state.bulk.cancel = true; });
    $('lang').addEventListener('click', () => { lang = lang === 'ja' ? 'en' : 'ja'; try { localStorage.setItem('rdsdr-lang', lang); } catch (e) { /* ok */ } applyStatic(); refresh(); });
    ['q', 'country', 'jponly', 'regfilter'].forEach(id => $(id).addEventListener(id === 'q' ? 'input' : 'change', renderCandidates));
    $('manualform').addEventListener('submit', (e) => {
      e.preventDefault();
      const f = new FormData($('manualform'));
      if (state.servers.length >= LIMITS.max) { toast(t('full'), true); return; }
      const host = String(f.get('host') || '').trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
      let name = String(f.get('name') || '').trim() || host;
      const s = { name, short: String(f.get('short') || '').trim() || shortFromName(name), host, port: Number(f.get('port') || 8073),
                  lat: String(f.get('lat') || '').trim() === '' ? null : Number(f.get('lat')), lon: String(f.get('lon') || '').trim() === '' ? null : Number(f.get('lon')),
                  password: '', password_set: false };
      s.short = uniqueShort(s.short, state.servers.map(x => x.short));
      const p = validateServer(s);
      if (p.length) { toast(p.map(problemText).join('\n'), true); return; }
      if (state.servers.some(x => keyOf(x) === keyOf(s))) { toast(t('v_duplicate'), true); return; }
      state.servers.push(s); $('manualform').reset(); $('manualform').port.value = 8073; refresh();
    });
    window.addEventListener('beforeunload', (e) => { if (isDirty()) { e.preventDefault(); e.returnValue = ''; } });
    setInterval(async () => {                                         // keep "in use" fresh and notice a stopped program
      try {
        const st = await api('/api/state'); state.running = st.running; state.current = st.current || null; $('banner').hidden = true; renderBar();
        const typing = document.activeElement && document.activeElement.closest && document.activeElement.closest('#registered') && document.activeElement.tagName === 'INPUT';
        if (!typing) renderRegistered();                                // do not rebuild the fields while the user types in one
        try { await loadHistory(); } catch (e) { /* ok */ }
        refreshChips();                                                // "3 min ago" moves on
      }
      catch (e) { const b = $('banner'); b.hidden = false; b.className = 'banner bad'; b.textContent = t('server_down'); }
    }, 5000);
  }

  wire();
  load(true).catch((e) => { const b = $('banner'); b.hidden = false; b.className = 'banner bad'; b.textContent = t('server_down') + ' ' + e.message; });
})();
