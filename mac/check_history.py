#!/usr/bin/env python3
"""Check history of the Receiver Manager: the last HTTP check and the last audio check of each receiver.

Why a separate file (check_history.json, next to servers.json) and not extra fields in servers.json:
  - servers.json is read by the Cardputer-facing program and by hand; its format must not change.
  - a check result is a log of what happened at one moment, not a setting.  It also covers candidates that are
    not registered, and it must survive reordering and unregistering (so it is keyed by host:port, not by list position).
  - losing or damaging this file costs nothing but the displayed history.  It is never needed to start the program.

Every failure here is swallowed on purpose (the history is a convenience); the caller only learns whether it was stored.
Format:
  {"version": 1, "entries": {"host:port": {"status": {...}, "audio": {...}}}}
  "status" = last HTTP check, "audio" = last audio check, each with checked_at (UTC, ISO 8601, "...Z") and the facts of that check.
"""
import json
import os
import tempfile
import threading
import time

VERSION = 1
MAX_ENTRIES = 600                       # about 5x the public list; the oldest are dropped first
LEVELS = ('status', 'audio')
# only these facts are kept (the page needs nothing else; this also bounds what a remote answer can put on disk)
KEEP = {
    'status': ('ok', 'checked_at', 'http', 'ms', 'status', 'offline', 'users', 'users_max', 'available', 'error'),
    'audio': ('ok', 'checked_at', 'in_use', 'blocks', 'sample_rate', 'elapsed', 'error'),
}


def now_iso():
    """UTC, second resolution, 'Z' suffix: unambiguous for any browser time zone."""
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def key_of(host, port):
    return '%s:%d' % (str(host).strip().lower(), int(port))


def _clip(v):
    if isinstance(v, str):
        return v[:160]
    if isinstance(v, (bool, int, float)) or v is None:
        return v
    return str(v)[:160]


def slim(level, result):
    return {k: _clip(result[k]) for k in KEEP[level] if k in result}


class CheckHistory:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.entries = {}
        self.note = ''                  # what happened while loading (shown nowhere critical; for logs/tests)
        self._load()

    # ---- reading
    def _load(self):
        try:
            with open(self.path, 'r', encoding='utf-8') as f:
                d = json.load(f)
            ents = d.get('entries') if isinstance(d, dict) else None
            if not isinstance(ents, dict):
                raise ValueError('no "entries" object')
            for k, v in ents.items():
                if isinstance(k, str) and isinstance(v, dict):
                    clean = {lv: v[lv] for lv in LEVELS if isinstance(v.get(lv), dict) and isinstance(v[lv].get('checked_at'), str)}
                    if clean:
                        self.entries[k] = clean
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:      # damaged: keep it aside, start empty (nothing else depends on it)
            self.entries = {}
            self.note = 'check history unreadable (%s)' % e
            try:
                os.replace(self.path, self.path + '.corrupt-' + time.strftime('%Y%m%d-%H%M%S'))
            except OSError:
                pass

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(self.entries))

    # ---- writing
    def record(self, host, port, level, result):
        """Store one finished check.  Returns True if it is now on disk."""
        if level not in LEVELS:
            return False
        item = slim(level, result)
        item.setdefault('checked_at', now_iso())
        with self.lock:
            e = self.entries.setdefault(key_of(host, port), {})
            e[level] = item
            self._prune()
            return self._write()

    def _prune(self):
        if len(self.entries) <= MAX_ENTRIES:
            return
        def newest(v):
            return max(x.get('checked_at', '') for x in v.values())
        for k in sorted(self.entries, key=lambda k: newest(self.entries[k]))[:len(self.entries) - MAX_ENTRIES]:
            del self.entries[k]

    def _write(self):
        tmp = None
        try:
            d = os.path.dirname(os.path.abspath(self.path))
            fd, tmp = tempfile.mkstemp(prefix='.check_history-', suffix='.tmp', dir=d)
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                json.dump({'version': VERSION, 'entries': self.entries}, f, ensure_ascii=False, indent=1, sort_keys=True)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.path)
            tmp = None
            return True
        except OSError as e:
            self.note = 'check history not saved (%s)' % e
            return False
        finally:
            if tmp:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
