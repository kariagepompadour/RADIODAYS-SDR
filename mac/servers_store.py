"""The receiver list (servers.json): validation and a safe store.

servers.json is a list of KiwiSDR receivers, in the order the Cardputer's globe shows them:

    [{"name": "Osaka, Japan (nr601sdrnet)", "short": "Osaka JP", "host": "nr601sdrnet.ddns.net", "port": 4400,
      "lat": 34.8585, "lon": 135.5144, "pos_source": "..."}, ...]

Rules (the Cardputer's font is ASCII only and its list holds 32 receivers):
  name  1..43 printable ASCII      short 1..20 printable ASCII (shown on the Cardputer)
  host  a host name or an IPv4 address (no "http://", no port, no path)      port 1..65535
  lat/lon  both or neither (neither = "position unverified": no marker on the globe)
  password (optional) for receivers that need one; stored as plain text, so servers.json is kept out of git
  free text kept as it is: pos_source, note, name_ja, id

Saving never leaves a damaged file behind: the new list is validated, written to a temporary file in the same folder,
read back and compared, and only then does it replace servers.json (os.replace is atomic).  The previous good file
is kept as servers.json.bak and as a time-stamped copy in .servers_backups/ (the newest 10); a damaged existing file
is set aside as servers.json.corrupt-<time> instead of being lost or overwriting the last good backup.
"""
import json
import os
import re
import shutil
import tempfile
import threading
import time

MAX_SERVERS = 32
NAME_MAX = 43
SHORT_MAX = 20
KEEP_BACKUPS = 10
EXTRA_TEXT_KEYS = ('pos_source', 'note', 'name_ja', 'id')

_HOST_RE = re.compile(r'^(?=.{1,253}$)[A-Za-z0-9]([A-Za-z0-9_-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9_-]{0,61}[A-Za-z0-9])?)*$')
_lock = threading.Lock()


class ServersError(ValueError):
    """The list (or the file) is not usable.  The message is meant to be shown to the user."""


def _ascii_text(value, what, max_len, index):
    s = str(value).strip()
    if not s:
        raise ServersError('#%d: %s is empty' % (index + 1, what))
    if len(s) > max_len:
        raise ServersError('#%d: %s is longer than %d characters (the Cardputer shows %d)' % (index + 1, what, max_len, max_len))
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in s):
        raise ServersError('#%d: %s must be plain ASCII (the Cardputer cannot show other characters)' % (index + 1, what))
    return s


def derive_short(name):
    """A short display name (<= 20 ASCII characters) from a long one."""
    s = ''.join(c for c in str(name) if 0x20 <= ord(c) <= 0x7E).strip()
    if '(' in s:
        s = s.split('(')[0].strip()
    s = s.replace(',', '').strip()
    return (s or 'Receiver')[:SHORT_MAX].rstrip()


def clean_server(s, index=0):
    """Validate one entry and return it in canonical form."""
    if not isinstance(s, dict):
        raise ServersError('#%d: not an object' % (index + 1))
    host = str(s.get('host', '')).strip()
    if not host:
        raise ServersError('#%d: host is empty' % (index + 1))
    if '://' in host or '/' in host or ':' in host or ' ' in host:
        raise ServersError('#%d: host must be a bare host name or IPv4 address (no http://, port or path): %r' % (index + 1, host[:60]))
    if not _HOST_RE.match(host):
        raise ServersError('#%d: not a valid host name: %r' % (index + 1, host[:60]))
    try:
        port = int(s.get('port', 8073))
    except (TypeError, ValueError):
        raise ServersError('#%d: port is not a number' % (index + 1))
    if not 1 <= port <= 65535:
        raise ServersError('#%d: port must be 1..65535' % (index + 1))
    name = _ascii_text(s.get('name') or host, 'name', NAME_MAX, index)
    short_in = s.get('short')
    short = _ascii_text(short_in if short_in not in (None, '') else derive_short(name), 'short name', SHORT_MAX, index)
    lat, lon = s.get('lat'), s.get('lon')
    if (lat is None) != (lon is None) and not (lat in ('', None) and lon in ('', None)):
        raise ServersError('#%d: give both latitude and longitude, or neither' % (index + 1))
    if lat in ('', None) or lon in ('', None):
        lat = lon = None
    else:
        try:
            lat, lon = float(lat), float(lon)
        except (TypeError, ValueError):
            raise ServersError('#%d: latitude/longitude are not numbers' % (index + 1))
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
            raise ServersError('#%d: latitude must be -90..90 and longitude -180..180' % (index + 1))
        lat, lon = round(lat, 6), round(lon, 6)
    password = str(s.get('password', '') or '')
    if len(password) > 64:
        raise ServersError('#%d: password is longer than 64 characters' % (index + 1))
    out = {'name': name, 'short': short, 'host': host, 'port': port, 'password': password, 'lat': lat, 'lon': lon}
    for k in EXTRA_TEXT_KEYS:
        v = s.get(k)
        if v not in (None, ''):
            out[k] = str(v)[:300]
    return out


def clean_servers(servers):
    """Validate the whole list (1..32 entries, no host:port twice) and return the canonical list."""
    if not isinstance(servers, list):
        raise ServersError('the receiver list must be a list')
    if not servers:
        raise ServersError('register at least one receiver')
    if len(servers) > MAX_SERVERS:
        raise ServersError('at most %d receivers can be registered (the Cardputer\'s list holds %d)' % (MAX_SERVERS, MAX_SERVERS))
    out, seen = [], {}
    for i, s in enumerate(servers):
        c = clean_server(s, i)
        key = (c['host'].lower(), c['port'])
        if key in seen:
            raise ServersError('#%d and #%d are the same receiver (%s:%d)' % (seen[key] + 1, i + 1, c['host'], c['port']))
        seen[key] = i
        out.append(c)
    return out


def to_file_form(c):
    """What is written to servers.json (empty optional values are left out)."""
    d = {'name': c['name'], 'short': c['short'], 'host': c['host'], 'port': c['port']}
    if c.get('lat') is not None:
        d['lat'], d['lon'] = c['lat'], c['lon']
    if c.get('password'):
        d['password'] = c['password']
    for k in EXTRA_TEXT_KEYS:
        if c.get(k):
            d[k] = c[k]
    return d


def _fsync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


class ServerStore:
    def __init__(self, path, default_path=None):
        self.path = os.path.abspath(path)
        self.default_path = default_path
        self.dir = os.path.dirname(self.path)
        self.bak = self.path + '.bak'
        self.backup_dir = os.path.join(self.dir, '.servers_backups')

    # ---- reading
    def _read(self, path):
        try:
            with open(path, encoding='utf-8') as f:
                data = json.load(f)
        except FileNotFoundError:
            raise ServersError('%s does not exist' % os.path.basename(path))
        except (OSError, ValueError) as e:
            raise ServersError('%s is not valid JSON: %s' % (os.path.basename(path), e))
        return clean_servers(data)

    def ensure(self):
        """First start: create servers.json from the shipped default list."""
        if not os.path.exists(self.path) and self.default_path and os.path.exists(self.default_path):
            shutil.copyfile(self.default_path, self.path)
            try:
                os.chmod(self.path, 0o600)
            except OSError:
                pass
            return True
        return False

    def load(self):
        self.ensure()
        return self._read(self.path)

    def load_with_fallback(self):
        """(servers, message).  A damaged servers.json falls back to the last good backup, then to the default list."""
        self.ensure()
        try:
            return self._read(self.path), ''
        except ServersError as e:
            problem = str(e)
        for label, p in (('the last good backup (servers.json.bak)', self.bak), ('the shipped default list', self.default_path)):
            if p and os.path.exists(p):
                try:
                    return self._read(p), 'WARNING: %s - using %s instead.  servers.json was NOT changed.' % (problem, label)
                except ServersError:
                    pass
        raise ServersError(problem)

    # ---- writing
    def save(self, servers):
        cleaned = clean_servers(servers)
        text = json.dumps([to_file_form(c) for c in cleaned], indent=2, ensure_ascii=False) + '\n'
        if clean_servers(json.loads(text)) != cleaned:      # what we would write must read back as what we were given
            raise ServersError('internal check failed: the list does not survive a round trip; nothing was written')
        with _lock:
            os.makedirs(self.dir, exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix='.servers.', suffix='.tmp', dir=self.dir)
            result = {'path': self.path, 'count': len(cleaned), 'backup': None, 'corrupt_copy': None}
            try:
                with os.fdopen(fd, 'w', encoding='utf-8') as f:
                    f.write(text)
                    f.flush()
                    os.fsync(f.fileno())
                os.chmod(tmp, 0o600)
                with open(tmp, encoding='utf-8') as rf:                                  # read the temporary file back
                    back = json.load(rf)
                if clean_servers(back) != cleaned:
                    raise ServersError('the temporary file did not read back correctly; servers.json was not changed')
                if os.path.exists(self.path):
                    stamp = time.strftime('%Y%m%d-%H%M%S')
                    try:
                        self._read(self.path)
                        good = True
                    except ServersError:
                        good = False
                    if good:
                        shutil.copyfile(self.path, self.bak + '.new')
                        os.replace(self.bak + '.new', self.bak)
                        os.makedirs(self.backup_dir, exist_ok=True)
                        shutil.copyfile(self.path, os.path.join(self.backup_dir, 'servers-%s.json' % stamp))
                        self._prune()
                        result['backup'] = self.bak
                    else:                                    # keep the damaged file for inspection; do not touch the last good .bak
                        keep = '%s.corrupt-%s' % (self.path, stamp)
                        shutil.copyfile(self.path, keep)
                        result['corrupt_copy'] = keep
                os.replace(tmp, self.path)                  # the atomic step
                tmp = None
                _fsync_dir(self.dir)
            finally:
                if tmp and os.path.exists(tmp):
                    os.unlink(tmp)
        return result

    def _prune(self):
        try:
            files = sorted(f for f in os.listdir(self.backup_dir) if f.startswith('servers-') and f.endswith('.json'))
            for f in files[:-KEEP_BACKUPS]:
                os.unlink(os.path.join(self.backup_dir, f))
        except OSError:
            pass

    def restore_backup(self):
        """Put the previous good list back (the current one becomes the new backup, so this can be undone)."""
        servers = self._read(self.bak)
        return self.save(servers), servers

    def info(self):
        def mt(p):
            try:
                return os.path.getmtime(p)
            except OSError:
                return None
        return {'file': os.path.basename(self.path), 'mtime': mt(self.path), 'backup_exists': os.path.exists(self.bak),
                'backup_mtime': mt(self.bak), 'max': MAX_SERVERS}
