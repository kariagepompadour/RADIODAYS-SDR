"""The few things that differ between macOS and Windows, kept in one place.

Everything else in RADIO DAYS SDR (KiwiSDR client, decoding, the Cardputer protocol, Receiver Manager, saving) is shared code.
What is here:
  - how the program tells which start file / port-checking command belongs to this OS (for messages),
  - the computer's LAN address,
  - socket address re-use (Windows SO_REUSEADDR lets a second program share a port; there it must stay off),
  - a console that never crashes on Japanese text (Windows code pages),
  - keeping Windows awake while the radio runs (macOS: the start file uses caffeinate).
Nothing here touches reception, decoding or saved data.
"""
import os
import socket
import sys
from http.server import ThreadingHTTPServer

IS_WINDOWS = sys.platform.startswith('win')
IS_MAC = sys.platform == 'darwin'

START_FILE = 'Start RADIO DAYS Win.bat' if IS_WINDOWS else 'Start RADIO DAYS Mac.command'
MANAGER_FILE = 'Open Receiver Manager Win.bat' if IS_WINDOWS else 'Open Receiver Manager Mac.command'


def reuse_address_wanted(platform=None):
    """SO_REUSEADDR is wanted on macOS/Linux (restart right after a stop).  On Windows it means something else - a second
    program may bind the same port - which would hide "already running" and "port in use", so it is not used there."""
    return not (platform or sys.platform).startswith('win')


def port_check_hint(port, platform=None):
    if (platform or sys.platform).startswith('win'):
        return 'netstat -ano | findstr :%d   （最後の列が PID。タスクマネージャーの「詳細」で確認 / the last column is the PID, see it in Task Manager > Details）' % port
    return 'lsof -nP -iTCP:%d -sTCP:LISTEN' % port


class LocalHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer whose "port already used" behaves the same on every OS (a second start must fail, not share the port)."""
    daemon_threads = True
    allow_reuse_address = reuse_address_wanted()

    def server_bind(self):
        if not reuse_address_wanted() and hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def prepare_listener(sock):
    """for the plain sockets (audio TCP, discovery UDP): re-use the address only where that is safe"""
    if reuse_address_wanted():
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    elif hasattr(socket, 'SO_EXCLUSIVEADDRUSE') and sock.type == socket.SOCK_STREAM:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)


def lan_ip():
    """The address of this computer on the LAN (what the Cardputer must reach), or '?'.  Nothing is sent."""
    for target in ('10.255.255.255', '192.0.2.1'):                 # a UDP connect only picks the outgoing interface
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((target, 1))
            ip = s.getsockname()[0]
            if ip and not ip.startswith('127.') and ip != '0.0.0.0':
                return ip
        except OSError:
            pass
        finally:
            s.close()
    try:                                                           # no default route: take any non-loopback address of this host
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith('127.'):
                return ip
    except OSError:
        pass
    return '?'


def setup_console():
    """Japanese/English messages must never raise UnicodeEncodeError on a console whose code page cannot show them."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='replace')
        except (AttributeError, ValueError, OSError):
            pass


def keep_awake():
    """Windows: ask not to idle-sleep while this process runs (released automatically when it ends).  Returns True if requested.
    macOS: the start file runs the program under `caffeinate -i`, so nothing to do here."""
    if not IS_WINDOWS:
        return False
    try:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        return bool(ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED))
    except Exception:
        return False
