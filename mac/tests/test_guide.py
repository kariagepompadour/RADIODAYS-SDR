"""Operation Guide tests: the guide (mac/manager/guide.json) must agree with the implementation and with docs/OPERATION_JA.md."""
import json
import os
import re
import subprocess
import sys
import unittest

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(MAC)
sys.path.insert(0, MAC)
import receiver_manager as rm  # noqa: E402
import servers_store as ss  # noqa: E402

def read(*parts):
    with open(os.path.join(*parts), encoding='utf-8') as f:
        return f.read()


GUIDE = json.loads(read(MAC, 'manager', 'guide.json'))
INO = read(ROOT, 'firmware', 'RadioDays_SDR', 'RadioDays_SDR.ino')
OPS = read(ROOT, 'docs', 'OPERATION_JA.md')


def walk(x):
    """every bilingual text in the guide as (path, lang, text)"""
    if isinstance(x, dict):
        if set(x) == {'ja', 'en'}:
            yield x
        else:
            for v in x.values():
                yield from walk(v)
    elif isinstance(x, list):
        for v in x:
            yield from walk(v)


def text(lang):
    return '\n'.join(t[lang] for t in walk(GUIDE['sections']))


def key_tokens():
    out = set()
    for s in GUIDE['sections']:
        for b in s['blocks']:
            if b['type'] == 'keys':
                for r in b['rows']:
                    out.update(k for k in r['keys'] if isinstance(k, str))
    return out


class GuideStructure(unittest.TestCase):
    def test_every_text_has_both_languages(self):
        n = 0
        for t in walk(GUIDE):
            n += 1
            self.assertTrue(t['ja'].strip() and t['en'].strip(), t)
        self.assertGreater(n, 50)
        ids = [s['id'] for s in GUIDE['sections']]
        self.assertEqual(len(ids), len(set(ids)))
        for need in ('basic', 'receiver', 'scan', 'output', 'manager', 'lamps', 'trouble'):    # what the request asked the guide to cover
            self.assertIn(need, ids)

    def test_default_language_is_english_and_the_choice_is_remembered(self):
        for name in ('app.js', 'guide.js'):
            src = open(os.path.join(MAC, 'manager', name), encoding='utf-8').read()
            m = re.search(r"let lang = \(function \(\) \{(.*?)\}\)\(\);", src, re.S)
            self.assertTrue(m, name)
            self.assertIn("rdsdr-lang", m.group(1))
            self.assertNotIn('navigator', m.group(1), name + ': the browser language must not decide')
            self.assertTrue(m.group(1).rstrip().endswith("return 'en';"), name)
        for name in ('index.html', 'guide.html'):
            self.assertIn('<html lang="en">', open(os.path.join(MAC, 'manager', name), encoding='utf-8').read())

    def test_generated_markdown_is_current(self):
        r = subprocess.run([sys.executable, os.path.join(MAC, 'tools', 'build_guide_docs.py'), '--check'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class GuideMatchesImplementation(unittest.TestCase):
    def test_every_key_the_firmware_handles_is_in_the_guide(self):
        body = INO[INO.index('static void handleRadio('):]
        body = body[:body.index('// ----', 1) if '// ----' in body[1:] else len(body)]
        handled = set(re.findall(r"case '(.)':", body[:body.index('case \'h\'') + 40]))
        handled -= {'=', '_'}                                   # aliases of + and -
        handled = {c.upper() if c.isalpha() else c for c in handled}
        guide = {k.upper() if len(k) == 1 and k.isalpha() else k for k in key_tokens()}
        self.assertGreater(len(handled), 20)
        self.assertEqual(sorted(handled - guide), [], 'keys handled by the firmware but missing in the guide')
        for k in ('Tab', 'ENTER', 'Esc'):
            self.assertIn(k, guide)
        self.assertIn('Tab', INO.replace('TAB', 'Tab'))

    def test_keys_in_guide_exist_in_operation_doc(self):
        for k in key_tokens():
            if k in ('Esc', '`'):
                self.assertIn('`Esc`', OPS)
            elif k == 'ENTER':
                self.assertIn('ENTER', OPS)
            else:
                self.assertTrue(re.search(r'`[^`\n]*%s[^`\n]*`' % re.escape(k), OPS) or re.search(r'`%s`' % re.escape(k), OPS, re.I), 'key %r not in OPERATION_JA.md' % k)

    def test_numbers_and_names_match_the_code(self):
        for lang in ('ja', 'en'):
            t = text(lang)
            self.assertIn('127.0.0.1:%d' % rm.DEFAULT_PORT, t)
            self.assertIn(str(ss.MAX_SERVERS), t)
            self.assertIn('%d' % ss.KEEP_BACKUPS, t)
            self.assertRegex(t, r'\b%d\b' % rm.BULK_CONCURRENCY)          # the bulk-check concurrency
            for name in ('check_history.json', 'servers.json.bak', '.servers_backups', 'Start RADIO DAYS Mac.command', 'Start RADIO DAYS Win.bat', 'Open Receiver Manager Mac.command', 'Open Receiver Manager Win.bat', '--manager-port 0', 'WiFi / PC / Kiwi'):
                self.assertIn(name, t, name)
        steps = re.search(r'STEPS\[\] = \{([^}]*)\}', INO).group(1)
        self.assertEqual([float(x.rstrip('f')) for x in steps.split(',')], [0.1, 1.0, 5.0, 10.0])
        self.assertIn('0.1 / 1 / 5 / 10 kHz', text('en'))
        self.assertIn('VOL_STEP = 5', INO)
        self.assertIn('±5', text('ja'))
        self.assertIn('txtT(163, 15, ROW_LINK, "PC")', INO)                    # full link labels in the colour-coded five-row panel
        self.assertIn('return mode == 0 ? 2 : 1', INO)                          # AM=5 kHz; USB/LSB=1 kHz
        self.assertIn('MANUAL', INO)
        for s in ('OUT PC', 'OUT ADV', 'PC NOT CONNECTED', 'END OF BAND', 'SCAN ENDED: KIWI LINK LOST'):
            self.assertIn(s, INO, s)
            self.assertIn(s, text('en'), s)
        self.assertEqual(rm.DEFAULT_PORT, 8780)

    def test_guide_does_not_overclaim(self):
        en = text('en')
        self.assertIn('do not guarantee', en)
        self.assertIn('保証するものではありません', text('ja'))


class GuideServed(unittest.TestCase):
    def test_package_files_exist(self):
        for f in ('guide.html', 'guide.js', 'guide.json'):
            self.assertTrue(os.path.exists(os.path.join(MAC, 'manager', f)), f)
        for p in ('/guide', '/guide.html', '/guide.js', '/guide.json'):
            self.assertIn(p, rm.STATIC)
        html = read(MAC, 'manager', 'index.html')
        self.assertRegex(html, r'<a id="guide"[^>]*href="/guide.html"[^>]*>WEB CONTROLLER</a>')
        labels = ('RECEIVER MANAGER', 'WEB CONTROLLER', 'OPERATION GUIDE')
        self.assertEqual(sorted(html.index(label) for label in labels), [html.index(label) for label in labels])
        self.assertNotRegex(html, r'(?i)window\.open|<iframe|target="_blank"')  # common navigation stays in one tab


if __name__ == '__main__':
    unittest.main()
