#!/usr/bin/env python3
"""Build manager/receiver_candidates.json - the list of public KiwiSDR receivers offered in the Receiver Manager.

The data comes from the public receiver directory of KiwiSDR.com (http://kiwisdr.com/public/).  This script reads a
machine-readable copy of that directory published by rx.linkfanel.net (the data behind a receiver map; its header says
"automatically generated from kiwisdr.com/public").  It is a SNAPSHOT of what the directory said at that moment:
a receiver being listed there does not mean it works for you now, so nothing here is marked "available".

    python3 tools/build_candidates.py                       # download and rebuild
    python3 tools/build_candidates.py --input list.js       # use a saved copy

What is included:
  * every active receiver in Japan, and
  * the best receivers per country (by the SNR they report), so that the list stays short and spread over the world.
What is left out: receivers that need a password / are offline in the directory, receivers on https only, and
receivers given as a bare IP address (usually someone's home connection; add those by hand if you know them).
Port: the directory shows a port only when it is not the default; KiwiSDR's default is 8073, which is assumed and
flagged (port_assumed) - the audio check in the Receiver Manager tells whether it works.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_URL = 'http://rx.linkfanel.net/kiwisdr_com.js'
DEFAULT_OUT = os.path.join(HERE, 'manager', 'receiver_candidates.json')
MUST_KEEP = {'nr601sdrnet.ddns.net:4400', 'sdr.rogerh.co.nz:8073', 'sdr.ironstonerange.com:8075', 'sdr.ironstonerange.com:8074'}
PER_COUNTRY = {'USA': 8, 'Germany': 6, 'UK': 6, 'Australia': 6, 'Canada': 5, 'New Zealand': 4}
DEFAULT_PER_COUNTRY = 3
CODES = {'Japan': 'JP', 'USA': 'US', 'UK': 'UK', 'Germany': 'DE', 'France': 'FR', 'Italy': 'IT', 'Spain': 'ES', 'Netherlands': 'NL',
         'Belgium': 'BE', 'Switzerland': 'CH', 'Austria': 'AT', 'Sweden': 'SE', 'Norway': 'NO', 'Finland': 'FI', 'Denmark': 'DK',
         'Poland': 'PL', 'Czech Republic': 'CZ', 'Hungary': 'HU', 'Ireland': 'IE', 'Portugal': 'PT', 'Australia': 'AU',
         'New Zealand': 'NZ', 'Canada': 'CA', 'Chile': 'CL', 'Brazil': 'BR', 'Argentina': 'AR', 'South Africa': 'ZA',
         'India': 'IN', 'China': 'CN', 'South Korea': 'KR', 'Taiwan': 'TW', 'Thailand': 'TH', 'Indonesia': 'ID',
         'Russia': 'RU', 'Ukraine': 'UA', 'Turkey': 'TR', 'Israel': 'IL', 'Greece': 'GR', 'Romania': 'RO', 'Bulgaria': 'BG',
         'Slovenia': 'SI', 'Croatia': 'HR', 'Serbia': 'RS', 'Slovakia': 'SK', 'Lithuania': 'LT', 'Latvia': 'LV', 'Estonia': 'EE',
         'Iceland': 'IS', 'Mexico': 'MX', 'Peru': 'PE', 'Colombia': 'CO', 'Philippines': 'PH', 'Malaysia': 'MY', 'Singapore': 'SG',
         'Vietnam': 'VN', 'Morocco': 'MA', 'Kenya': 'KE', 'Namibia': 'NA', 'Mongolia': 'MN', 'Kazakhstan': 'KZ'}
RULES = [  # (substring of the lower-cased location, country); first match wins
    ('japan', 'Japan'), ('new zealand', 'New Zealand'), ('south australia', 'Australia'), ('western australia', 'Australia'),
    ('australia', 'Australia'), ('queensland', 'Australia'), ('tasmania', 'Australia'), ('victoria, au', 'Australia'),
    ('united kingdom', 'UK'), ('england', 'UK'), ('scotland', 'UK'), ('wales', 'UK'), (' uk', 'UK'), ('germany', 'Germany'),
    ('deutschland', 'Germany'), ('france', 'France'), ('italy', 'Italy'), ('italia', 'Italy'), ('spain', 'Spain'),
    ('netherlands', 'Netherlands'), ('belgium', 'Belgium'), ('switzerland', 'Switzerland'), ('austria', 'Austria'),
    ('sweden', 'Sweden'), ('norway', 'Norway'), ('finland', 'Finland'), ('denmark', 'Denmark'), ('poland', 'Poland'),
    ('czech', 'Czech Republic'), ('hungary', 'Hungary'), ('ireland', 'Ireland'), ('portugal', 'Portugal'), ('canada', 'Canada'),
    ('chile', 'Chile'), ('brazil', 'Brazil'), ('argentina', 'Argentina'), ('south africa', 'South Africa'), ('india', 'India'),
    ('china', 'China'), ('korea', 'South Korea'), ('taiwan', 'Taiwan'), ('thailand', 'Thailand'), ('indonesia', 'Indonesia'),
    ('russia', 'Russia'), ('ukraine', 'Ukraine'), ('turkey', 'Turkey'), ('israel', 'Israel'), ('greece', 'Greece'),
    ('romania', 'Romania'), ('bulgaria', 'Bulgaria'), ('slovenia', 'Slovenia'), ('croatia', 'Croatia'), ('serbia', 'Serbia'),
    ('slovakia', 'Slovakia'), ('lithuania', 'Lithuania'), ('latvia', 'Latvia'), ('estonia', 'Estonia'), ('iceland', 'Iceland'),
    ('mexico', 'Mexico'), ('peru', 'Peru'), ('colombia', 'Colombia'), ('philippines', 'Philippines'), ('malaysia', 'Malaysia'),
    ('singapore', 'Singapore'), ('vietnam', 'Vietnam'), ('morocco', 'Morocco'), ('kenya', 'Kenya'), ('namibia', 'Namibia'),
    ('mongolia', 'Mongolia'), ('kazakhstan', 'Kazakhstan'), ('usa', 'USA'), ('united states', 'USA')]


def ascii_text(s, limit):
    s = ''.join(c if 0x20 <= ord(c) <= 0x7E else ' ' for c in str(s))
    return re.sub(r'\s+', ' ', s).strip()[:limit].strip()


def country_of(loc, lat, lon):
    l = ' ' + loc.lower() + ' '
    for key, c in RULES:
        if key in l:
            return c
    if lat is not None and 24 <= lat <= 50 and -125 <= lon <= -66:        # a US state code such as "CA" or "WA"
        return 'USA'
    if lat is not None and 42 <= lat <= 70 and -141 <= lon <= -52:
        return 'Canada'
    return 'Other'


def host_label(host):
    """A short, telling label for a host name: ja7kbr for ja7kbr.proxy.kiwisdr.com, hirokinet for kiwisdr.hirokinet.com."""
    parts = host.split('.')
    if len(parts) > 2 and parts[0].lower() in ('kiwisdr', 'kiwi', 'sdr', 'websdr', 'www') and not host.endswith('.proxy.kiwisdr.com'):
        return parts[1]
    return parts[0]


def parse_gps(g):
    m = re.match(r'\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)', str(g or ''))
    if not m:
        return None, None
    lat, lon = float(m.group(1)), float(m.group(2))
    if (lat, lon) == (0.0, 0.0) or abs(lat) > 90 or abs(lon) > 180:
        return None, None
    return round(lat, 4), round(lon, 4)


def first_int(s, default=0):
    m = re.match(r'\s*(\d+)', str(s or ''))
    return int(m.group(1)) if m else default


def load_directory(text):
    head = text[:600]
    stamp = re.search(r'data timestamp:\s*(.+)', head)
    body = text[text.index('['):text.rindex(']') + 1]
    body = re.sub(r',\s*([\]}])', r'\1', body)            # tolerate trailing commas
    return json.loads(body), (stamp.group(1).strip() if stamp else 'unknown')


def build(entries, max_total=170):
    cands = []
    for r in entries:
        if r.get('status') != 'active' or r.get('offline') != 'no' or str(r.get('ant_connected', '1')) == '0':
            continue
        m = re.match(r'^http://([A-Za-z0-9._-]+)(?::(\d+))?/?$', r.get('url', '').strip())
        if not m:
            continue                                         # https only, or something odd
        host, port_s = m.group(1), m.group(2)
        if re.match(r'^\d+\.\d+\.\d+\.\d+$', host):
            continue                                         # a bare IP address
        lat, lon = parse_gps(r.get('gps'))
        loc = ascii_text(r.get('loc', ''), 60) or ascii_text(r.get('name', ''), 60)
        country = country_of(loc, lat, lon)
        bm = re.match(r'(\d+)-(\d+)', str(r.get('bands', '')))
        lo, hi = (int(bm.group(1)), int(bm.group(2))) if bm else (0, 0)
        snr = first_int(r.get('snr'))
        port = int(port_s) if port_s else 8073
        cands.append({'host': host, 'port': port, 'port_assumed': port_s is None, 'loc': loc, 'country': country,
                      'lat': lat, 'lon': lon, 'snr': snr, 'users_max': first_int(r.get('users_max')),
                      'bands_khz': [lo // 1000, hi // 1000], 'antenna': ascii_text(r.get('antenna', ''), 60),
                      'sw': ascii_text(r.get('sw_version', ''), 20).replace('KiwiSDR_', ''),
                      'directory_updated': ascii_text(r.get('updated', ''), 40), '_hf': lo <= 5900000 and hi >= 15000000})
    chosen, per, seen = [], {}, set()
    for c in sorted(cands, key=lambda c: -c['snr']):
        key = '%s:%d' % (c['host'], c['port'])
        if key in seen:                                      # the directory can list one receiver twice: keep the best entry
            continue
        seen.add(key)
        keep = key in MUST_KEEP or c['country'] == 'Japan'
        if not keep and c['_hf'] and c['snr'] >= 20 and per.get(c['country'], 0) < PER_COUNTRY.get(c['country'], DEFAULT_PER_COUNTRY):
            keep = True
        if keep:
            per[c['country']] = per.get(c['country'], 0) + 1
            chosen.append(c)
    chosen = chosen[:max_total] if len(chosen) > max_total else chosen
    used_short, out = set(), []
    for c in sorted(chosen, key=lambda c: (c['country'] != 'Japan', c['country'], c['loc'].lower(), c['host'])):
        label = host_label(c['host'])
        city = ascii_text(c['loc'].split(',')[0], 14) or label
        code = CODES.get(c['country'], '--')
        short = ('%s %s' % (city, code))[:20].strip()
        if short in used_short:
            short = ('%s %s' % (city[:12], label[:7]))[:20].strip()
        n = 2
        while short in used_short:
            short = ('%s %d' % (short[:17], n))[:20]
            n += 1
        used_short.add(short)
        name = ('%s (%s)' % (c['loc'], label))
        if len(name) > 43:
            name = ('%s (%s)' % (c['loc'][:43 - len(label) - 3].rstrip(' ,'), label))
        item = {'id': '%s:%d' % (c['host'], c['port']), 'name': name, 'short': short}
        for k in ('host', 'port', 'port_assumed', 'loc', 'country', 'lat', 'lon', 'snr', 'users_max', 'bands_khz', 'antenna', 'sw', 'directory_updated'):
            item[k] = c[k]
        out.append(item)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--input', help='a saved copy of the directory (default: download)')
    ap.add_argument('--url', default=DEFAULT_URL)
    ap.add_argument('--output', default=DEFAULT_OUT)
    ap.add_argument('--max', type=int, default=170)
    args = ap.parse_args()
    if args.input:
        text = open(args.input, encoding='utf-8').read()
        src = os.path.basename(args.input)
    else:
        with urllib.request.urlopen(args.url, timeout=60) as r:
            text = r.read().decode('utf-8', 'replace')
        src = args.url
    entries, stamp = load_directory(text)
    receivers = build(entries, args.max)
    doc = {'about': 'A SNAPSHOT of the public KiwiSDR receiver directory (kiwisdr.com/public). A receiver in this list is NOT known to work now: '
                    'availability changes, some receivers limit connection time or users, and some need a password. '
                    'Use the checks in the Receiver Manager and respect each receiver owner\'s rules.',
           'source': 'KiwiSDR.com public receiver directory, via ' + src, 'directory_timestamp': stamp,
           'built': time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime()), 'selection': 'all active receivers in Japan + the best (by reported SNR) per country; '
           'excluded: https-only, bare IP addresses; port 8073 is assumed when the directory shows none (port_assumed)',
           'count': len(receivers), 'receivers': receivers}
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=1, ensure_ascii=True)
        f.write('\n')
    jp = sum(1 for r in receivers if r['country'] == 'Japan')
    print('%d receivers (%d in Japan) from %d directory entries; directory timestamp: %s' % (len(receivers), jp, len(entries), stamp))
    return 0


if __name__ == '__main__':
    sys.exit(main())
