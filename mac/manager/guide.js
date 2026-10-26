/* RADIO DAYS SDR - Operation Guide page.  The text is not in this file: it is guide.json (the single source, also used to
 * generate docs/QUICK_GUIDE_*.md by mac/tools/build_guide_docs.py).  Plain DOM, no libraries, no network except /guide.json. */
'use strict';
(function () {
  const UI = { ja: { start: '最初にここから', detail: '詳しい説明: ', failed: '操作ガイドを読み込めませんでした。' },
               en: { start: 'START HERE', detail: 'More detail: ', failed: 'Could not load the operation guide.' } };
  let lang = (function () { try { const l = localStorage.getItem('rdsdr-lang'); if (l === 'ja' || l === 'en') return l; } catch (e) { /* no storage */ } return 'en'; })();   // English unless the user chose Japanese (the choice is remembered)
  let guide = null;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  const tx = (v) => (v && typeof v === 'object') ? (v[lang] || v.en || '') : (v || '');

  function renderBlock(b) {
    if (b.type === 'p' && b.emphasis === 'start') { const d = el('div', 'start-here'); d.append(el('strong', '', UI[lang].start), el('p', '', tx(b.text))); return d; }
    if (b.type === 'p') return el('p', '', tx(b.text));
    if (b.type === 'h') return el('h3', '', tx(b.text));
    if (b.type === 'list') { const ul = el('ul'); b.items.forEach(i => ul.append(el('li', '', tx(i)))); return ul; }
    if (b.type === 'keys') {
      const t = el('table', 'keys'); const tb = el('tbody');
      b.rows.forEach(r => {
        const tr = el('tr'); const k = el('td', 'k');
        r.keys.forEach((x, i) => { if (i) k.append(' '); k.append(el('kbd', '', tx(x))); });
        tr.append(k, el('td', '', tx(r.text))); tb.append(tr);
      });
      t.append(tb); return t;
    }
    if (b.type === 'table') {
      const t = el('table', 'plain'); const th = el('thead'); const hr = el('tr');
      b.head.forEach(h => hr.append(el('th', '', tx(h)))); th.append(hr); t.append(th);
      const tb = el('tbody'); b.rows.forEach(r => { const tr = el('tr'); r.forEach(c => tr.append(el('td', '', tx(c)))); tb.append(tr); }); t.append(tb);
      return t;
    }
    return el('span');
  }

  function render() {
    document.documentElement.lang = lang;
    if (!guide) { $('content').textContent = UI[lang].failed; return; }
    document.title = 'RADIO DAYS SDR - ' + tx(guide.title);
    $('gtitle').textContent = tx(guide.title);
    $('gsub').textContent = tx(guide.subtitle) + ' · ' + tx(guide.intro);
    const toc = $('toc'), box = $('content'); toc.textContent = ''; box.textContent = '';
    guide.sections.forEach(s => {
      const a = el('a', '', tx(s.title)); a.href = '#' + s.id; toc.append(a);
      const sec = el('section', 'gsec'); sec.id = s.id; sec.append(el('h2', '', tx(s.title)));
      s.blocks.forEach(b => sec.append(renderBlock(b)));
      box.append(sec);
    });
    const f = $('gfoot'); f.textContent = UI[lang].detail; f.append(el('code', '', guide.detail_doc));
  }

  $('lang').addEventListener('click', () => { lang = lang === 'ja' ? 'en' : 'ja'; try { localStorage.setItem('rdsdr-lang', lang); } catch (e) { /* ok */ } render(); });
  fetch('/guide.json').then(r => r.json()).then(j => { guide = j; render(); }).catch(() => render());
  render();
})();
