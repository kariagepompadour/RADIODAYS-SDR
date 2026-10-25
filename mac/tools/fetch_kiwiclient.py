#!/usr/bin/env python3
"""Download the KiwiSDR client library that RADIO DAYS SDR uses (jks-prv/kiwiclient) into vendor/kiwiclient/.

Why a download and not a copy in this repository: the upstream project does not state a license for most of its files,
and kiwi/rigctld.py is GPL-2.0-or-later.  So it is fetched from the author's repository on your computer, at one pinned
commit (the one this program was tested with), and every file is checked against a SHA-256 list before it is used.

    python3 tools/fetch_kiwiclient.py            # fetch if missing (or if a file does not match)
    python3 tools/fetch_kiwiclient.py --force    # fetch again

Offline?  Get https://github.com/jks-prv/kiwiclient at the commit below and copy its kiwi/, mod_pywebsocket/ and chunk/
folders into mac/vendor/kiwiclient/ .
"""
import hashlib
import io
import os
import shutil
import sys
import urllib.request
import zipfile

COMMIT = '4eb733e6b6147f7fbeb97ced64cdac029b202d18'
URL = 'https://github.com/jks-prv/kiwiclient/archive/%s.zip' % COMMIT
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(HERE, 'vendor', 'kiwiclient')
EXPECTED = {
    'chunk/COPIED_FROM.txt': '4212cdc30281f557c808cebc2038182aa41ee50433f416307b97dadf533eeb8c',
    'chunk/LICENSE': '14949dab294d8379384238869abc035f4ef95bb1968b10f88554afa001ead28e',
    'chunk/README.rst': '589093ef667bbd79f44a5d6a805352d31fb68c328b56be498c3af85377d890d6',
    'chunk/__init__.py': 'de5e5f320a8f38e8d3fa6f6536b244d174fa501563b79a465c3f57e3c077ef7e',
    'kiwi/__init__.py': '432b8a6f8e51f57c92ce15d5560d0c027aa5202ad4b0f51b9a34f6ece5a1a84e',
    'kiwi/client.py': '1eff86798a5cad8be20e90baafc569483bab82021e34429a96e44941594353cd',
    'kiwi/rigctld.py': '57a02e7b17d374e57a4441444579137b940ebf007074326f5e06220194764e2e',
    'kiwi/wavreader.py': '1541bf5fb2f68f7b866654d373aff9d6e87e9554320d6f18148a219f92a7af0f',
    'kiwi/worker.py': 'd076b3ab78e8fcb1da1ff0ba58b9d23936a61241164a0a45464d1ceab9581480',
    'kiwi/wsclient.py': 'd8e78de980c6ebba38aca6d8d78efaea09b5b1d2e5c68d514d3c4543c45730c3',
    'mod_pywebsocket/__init__.py': 'b5d3dbc6ce9d70fff1bd25cc02f3f05608884b2dfef8e70c9dfaa4be68390061',
    'mod_pywebsocket/_stream_base.py': 'f8248a97a49c02129e15893d28e6e3a5320036ad4332292011ed96f74f0d57e0',
    'mod_pywebsocket/_stream_hixie75.py': 'ce88f7abd114a5ab6eeb0c05df85a46288b3bef72c0e03ccab63178c2d6081cd',
    'mod_pywebsocket/_stream_hybi.py': 'dc1ad4ccae115b0767d5c9756988501aa8e8d7dd31541e1dbc786d1557805ce3',
    'mod_pywebsocket/common.py': 'e43cc5e4a1f5c61431f082f3e6bc920a768357218d7f001ff1191fd64f04311a',
    'mod_pywebsocket/extensions.py': 'f29d3cbd08968c5789408309556fd2ca09b1ec41140761fdcd4f9d317f8d6309',
    'mod_pywebsocket/http_header_util.py': '6d1fdee0025c6e68301b8347a42302537b3e21881a75e956fb8b8be1bb289c9d',
    'mod_pywebsocket/stream.py': 'ece3dc9b1505fb2b7975149a996388a9e4b1ed25e337afea28793fe1b70cfbdc',
    'mod_pywebsocket/util.py': 'c0b49c42da174bbe9c8e44044f4c9b403f8e5f84286d20ce7cc7363206b10031'
}


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def verify():
    """[] when every expected file is present and unchanged, else the list of problems."""
    bad = []
    for rel, digest in EXPECTED.items():
        p = os.path.join(DEST, rel)
        if not os.path.exists(p):
            bad.append('missing: ' + rel)
        elif sha256(p) != digest:
            bad.append('changed: ' + rel)
    return bad


def fetch():
    print('Downloading KiwiClient %s from github.com/jks-prv/kiwiclient ...' % COMMIT[:10])
    try:
        with urllib.request.urlopen(URL, timeout=60) as r:
            data = r.read()
    except OSError as e:
        sys.exit('Download failed: %s\nSee the note at the top of this file for a manual install.' % e)
    z = zipfile.ZipFile(io.BytesIO(data))
    root = z.namelist()[0].split('/')[0]
    tmp = DEST + '.new'
    shutil.rmtree(tmp, ignore_errors=True)
    for name in z.namelist():
        rel = name[len(root) + 1:]
        if rel.split('/')[0] in ('kiwi', 'mod_pywebsocket', 'chunk') and not name.endswith('/'):
            target = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as f:
                f.write(z.read(name))
    for rel, digest in EXPECTED.items():
        p = os.path.join(tmp, rel)
        if not os.path.exists(p) or sha256(p) != digest:
            shutil.rmtree(tmp, ignore_errors=True)
            sys.exit('The downloaded file does not match the expected checksum (%s). Nothing was installed.' % rel)
    shutil.rmtree(DEST, ignore_errors=True)
    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    os.rename(tmp, DEST)
    print('KiwiClient installed in', DEST)


def main():
    force = '--force' in sys.argv
    problems = [] if force is False else ['forced']
    if not force:
        problems = verify()
    if problems:
        fetch()
        bad = verify()
        if bad:
            sys.exit('After the download: ' + '; '.join(bad[:3]))
    else:
        print('KiwiClient is installed and matches the pinned commit', COMMIT[:10])
    return 0


if __name__ == '__main__':
    sys.exit(main())
