#!/usr/bin/env python3
"""Print the position a KiwiSDR reports about itself, for servers.json.

  python3 kiwi_position.py sdr.example.com:8073

Reads http://HOST:PORT/status (the "gps=(lat, lon)" line).  Cross-check with the public list
(http://kiwisdr.com/public/) before trusting it; leave lat/lon out of servers.json when unsure.
"""
import re
import sys
import urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
try:
    text = urllib.request.urlopen('http://%s/status' % host, timeout=10).read().decode('utf-8', 'replace')
except OSError as e:
    sys.exit('cannot read status: %s' % e)
m = re.search(r'^gps=\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)', text, re.M)
name = re.search(r'^name=(.*)$', text, re.M)
loc = re.search(r'^loc=(.*)$', text, re.M)
print('name:', name.group(1).strip() if name else '?')
print('loc :', loc.group(1).strip() if loc else '?')
if m and not (float(m.group(1)) == 0 and float(m.group(2)) == 0):
    print('"lat": %s, "lon": %s' % (m.group(1), m.group(2)))
else:
    print('no usable position reported - leave lat/lon out (shown as unverified)')
