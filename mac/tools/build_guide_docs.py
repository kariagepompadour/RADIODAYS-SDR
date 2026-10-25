#!/usr/bin/env python3
"""Generate docs/QUICK_GUIDE_JA.md and docs/QUICK_GUIDE_EN.md from mac/manager/guide.json.

guide.json is the single source of the Operation Guide: the Receiver Manager page (/guide.html) renders it in the browser,
and this script renders the same content as Markdown for GitHub.  Edit guide.json, then run:

    python3 mac/tools/build_guide_docs.py          # rewrite the two Markdown files
    python3 mac/tools/build_guide_docs.py --check  # exit 1 if they are out of date (the tests do this)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GUIDE = os.path.join(ROOT, 'mac', 'manager', 'guide.json')
OUT = {'ja': os.path.join(ROOT, 'docs', 'QUICK_GUIDE_JA.md'), 'en': os.path.join(ROOT, 'docs', 'QUICK_GUIDE_EN.md')}
HEADER = {'ja': '<!-- このファイルは mac/manager/guide.json から mac/tools/build_guide_docs.py で生成されます。直接編集しないでください。 -->',
          'en': '<!-- Generated from mac/manager/guide.json by mac/tools/build_guide_docs.py. Do not edit by hand. -->'}
MORE = {'ja': '詳しい説明: [OPERATION_JA.md](OPERATION_JA.md)（日本語）', 'en': 'More detail: [OPERATION_JA.md](OPERATION_JA.md) (Japanese)'}
OTHER = {'ja': '[English](QUICK_GUIDE_EN.md)', 'en': '[日本語](QUICK_GUIDE_JA.md)'}


def tx(v, lang):
    return v[lang] if isinstance(v, dict) else v


def cell(s):
    return str(s).replace('|', '\\|').replace('\n', ' ')


def code(k):
    return '`` %s ``' % k if '`' in k else '`%s`' % cell(k)


def render(guide, lang):
    out = [HEADER[lang], '', '# RADIO DAYS SDR - ' + tx(guide['title'], lang), '', '**' + tx(guide['subtitle'], lang) + '** · ' + OTHER[lang], '',
           tx(guide['intro'], lang), '']
    for s in guide['sections']:
        out += ['## ' + tx(s['title'], lang), '']
        for b in s['blocks']:
            t = b['type']
            if t == 'p':
                out += [tx(b['text'], lang), '']
            elif t == 'h':
                out += ['### ' + tx(b['text'], lang), '']
            elif t == 'list':
                out += ['- ' + tx(i, lang) for i in b['items']] + ['']
            elif t == 'keys':
                out += ['| ' + ('キー・操作' if lang == 'ja' else 'Key / action') + ' | |', '|---|---|']
                for r in b['rows']:
                    out.append('| ' + ' '.join(code(k) if isinstance(k, str) else cell(tx(k, lang)) for k in r['keys']) + ' | ' + cell(tx(r['text'], lang)) + ' |')
                out.append('')
            elif t == 'table':
                out += ['| ' + ' | '.join(cell(tx(h, lang)) for h in b['head']) + ' |', '|' + '---|' * len(b['head'])]
                out += ['| ' + ' | '.join(cell(tx(c, lang)) for c in r) + ' |' for r in b['rows']] + ['']
            else:
                raise ValueError('unknown block type %r' % t)
    out += ['---', MORE[lang], '']
    return '\n'.join(out)


def main():
    with open(GUIDE, encoding='utf-8') as f:
        guide = json.load(f)
    stale = []
    for lang, path in OUT.items():
        text = render(guide, lang)
        if '--check' in sys.argv:
            try:
                with open(path, encoding='utf-8') as f:
                    if f.read() != text:
                        stale.append(path)
            except OSError:
                stale.append(path)
        else:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(text)
            print('wrote', os.path.relpath(path, ROOT))
    if stale:
        print('out of date (run mac/tools/build_guide_docs.py):', *[os.path.relpath(p, ROOT) for p in stale])
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
