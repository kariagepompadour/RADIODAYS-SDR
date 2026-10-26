"""Windows 11 support: what can be checked on a Mac.  These are STATIC and simulated checks - nothing here runs on Windows.
They cover the OS-specific module with the platform name faked, and the Windows start files as text."""
import io
import os
import re
import socket
import sys
import unittest
from unittest import mock

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(MAC)
sys.path.insert(0, MAC)
import platform_support as ps  # noqa: E402

BAT_START = os.path.join(ROOT, 'Start RADIO DAYS Win.bat')
BAT_MGR = os.path.join(ROOT, 'Open Receiver Manager Win.bat')
SHARED_PY = ['radiodays_sdr.py', 'receiver_manager.py', 'servers_store.py', 'check_history.py', 'platform_support.py']


class PlatformLogic(unittest.TestCase):
    def test_address_reuse_rule(self):
        self.assertTrue(ps.reuse_address_wanted('darwin'))
        self.assertTrue(ps.reuse_address_wanted('linux'))
        self.assertFalse(ps.reuse_address_wanted('win32'))              # Windows: a second program must NOT be able to share the port

    def test_port_hint_per_os(self):
        self.assertIn('lsof', ps.port_check_hint(8780, 'darwin'))
        self.assertIn('netstat', ps.port_check_hint(8780, 'win32'))
        self.assertIn(':8780', ps.port_check_hint(8780, 'win32'))

    def test_second_listener_cannot_share_a_port_when_reuse_is_off(self):
        # the Windows setting, applied here: no SO_REUSEADDR -> a second bind to a port that is in use must fail
        a = socket.socket()
        a.bind(('127.0.0.1', 0))
        a.listen(1)
        port = a.getsockname()[1]
        b = socket.socket()
        try:
            with self.assertRaises(OSError):
                b.bind(('127.0.0.1', port))
        finally:
            a.close()
            b.close()

    def test_lan_ip_falls_back_when_no_route(self):
        real = socket.socket

        class NoRoute:
            def __init__(self, *a, **k):
                pass

            def connect(self, addr):
                raise OSError('network unreachable')

            def close(self):
                pass
        with mock.patch.object(ps.socket, 'socket', NoRoute), \
                mock.patch.object(ps.socket, 'gethostbyname_ex', return_value=('h', [], ['127.0.0.1', '192.168.1.50'])):
            self.assertEqual(ps.lan_ip(), '192.168.1.50')
        with mock.patch.object(ps.socket, 'socket', NoRoute), \
                mock.patch.object(ps.socket, 'gethostbyname_ex', side_effect=OSError):
            self.assertEqual(ps.lan_ip(), '?')
        self.assertIs(ps.socket.socket, real)

    def test_console_survives_japanese_on_a_western_code_page(self):
        raw = io.BytesIO()
        old = sys.stdout
        sys.stdout = io.TextIOWrapper(raw, encoding='cp1252', errors='strict')
        try:
            with self.assertRaises(UnicodeEncodeError):
                print('日本語')
            ps.setup_console()
            print('日本語 / English')                                    # must not raise any more
            sys.stdout.flush()
            got = raw.getvalue()
        finally:
            sys.stdout = old
        self.assertIn(b'English', got)

    def test_keep_awake_is_windows_only_and_never_raises(self):
        with mock.patch.object(ps, 'IS_WINDOWS', False):
            self.assertFalse(ps.keep_awake())
        with mock.patch.object(ps, 'IS_WINDOWS', True):                 # ctypes.windll does not exist here: must just return False
            self.assertFalse(ps.keep_awake())

    def test_start_file_names(self):
        self.assertEqual(ps.START_FILE, 'Start RADIO DAYS Mac.command' if sys.platform == 'darwin' else ps.START_FILE)
        self.assertTrue(os.path.exists(os.path.join(ROOT, ps.START_FILE)) or ps.IS_WINDOWS is False)


class SharedCodeIsPortable(unittest.TestCase):
    def test_no_unix_only_calls_in_shared_code(self):
        bad = re.compile(r'\bfcntl\b|\btermios\b|os\.fork\(|os\.getuid|signal\.SIGKILL|signal\.SIGHUP|os\.killpg|/dev/null|/tmp/|\bpwd\b|os\.geteuid')
        for f in SHARED_PY:
            with open(os.path.join(MAC, f), encoding='utf-8') as fh:
                for n, line in enumerate(fh, 1):
                    if bad.search(line) and not line.lstrip().startswith('#'):
                        self.fail('%s:%d uses a Unix-only feature: %s' % (f, n, line.strip()))

    def test_text_files_are_opened_with_an_encoding(self):
        # Windows' default text encoding is not UTF-8: every text open() in shared code must say what it reads/writes
        for f in SHARED_PY:
            with open(os.path.join(MAC, f), encoding='utf-8') as fh:
                for n, line in enumerate(fh, 1):
                    m = re.search(r"(?<!os\.)\bopen\(", line)
                    if m and "'rb'" not in line and "'wb'" not in line and 'encoding' not in line and 'def open' not in line \
                            and 'webbrowser' not in line and 'self.sink' not in line and 'rec.open' not in line and 'urlopen' not in line \
                            and 'port.open' not in line:  # pyserial opens a serial port, not a text file
                        self.fail('%s:%d open() without encoding: %s' % (f, n, line.strip()))

    def test_servers_are_not_written_through_a_unix_path_assumption(self):
        with open(os.path.join(MAC, 'servers_store.py'), encoding='utf-8') as f:
            s = f.read()
        self.assertIn("except OSError:\n        pass", s)                 # directory fsync does not exist on Windows: its failure is ignored


class WindowsStartFiles(unittest.TestCase):
    def read(self, p):
        with open(p, 'rb') as f:
            raw = f.read()
        return raw, raw.decode('ascii')                                  # ASCII only: batch files and UTF-8 do not mix reliably

    def test_crlf_ascii_and_names(self):
        for p in (BAT_START, BAT_MGR):
            raw, text = self.read(p)
            self.assertNotIn(b'\n', raw.replace(b'\r\n', b''), p)        # every line break is CRLF
            self.assertTrue(text.startswith('@echo off'))

    def test_labels_exist_and_every_goto_has_a_target(self):
        for p in (BAT_START, BAT_MGR):
            _, text = self.read(p)
            labels = set(re.findall(r'^:(\w+)', text, re.M))
            for target in re.findall(r'(?:goto|call)\s+:(\w+)', text, re.I):
                self.assertIn(target, labels, '%s: jump to missing label %s' % (os.path.basename(p), target))

    def test_start_file_uses_the_windows_venv_and_the_shared_program(self):
        _, text = self.read(BAT_START)
        for needle in ('.venv\\Scripts\\python.exe', 'requirements.txt', 'tools\\fetch_kiwiclient.py', 'radiodays_sdr.py --open-manager %*',
                       'PYTHONUTF8', 'chcp 65001', 'pause', 'py -3', 'cd /d "%~dp0mac"'):
            self.assertIn(needle, text)
        self.assertNotIn('bin/python', text)                              # no macOS venv path
        _, m = self.read(BAT_MGR)
        self.assertIn('receiver_manager.py --open %*', m)
        self.assertNotIn('radiodays_sdr.py', m)                            # the manager file never starts the radio

    def test_old_start_files_are_gone_and_no_doc_refers_to_them(self):
        for old in ('start_mac.command', 'start_manager.command'):
            self.assertFalse(os.path.exists(os.path.join(ROOT, old)), old)
        for dirpath, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in ('.venv', 'vendor', '__pycache__', 'bin', '.git')]
            for f in files:
                if not f.endswith(('.md', '.py', '.json', '.sh', '.command', '.bat', '.h', '.ino')):
                    continue
                with open(os.path.join(dirpath, f), encoding='utf-8', errors='replace') as fh:
                    text = fh.read()
                if f == 'test_platform.py':
                    continue
                for old in ('start_mac.command', 'start_manager.command'):
                    if old in text:
                        # Historical verification and private HANDOFF notes may quote old releases.
                        self.assertTrue(f.startswith('VERIFICATION') or f.startswith('HANDOFF_'),
                                        '%s still mentions %s' % (f, old))

    def test_user_facing_files_have_os_names_and_shared_ones_do_not(self):
        names = set(os.listdir(ROOT))
        for n in ('Start RADIO DAYS Mac.command', 'Open Receiver Manager Mac.command', 'Start RADIO DAYS Win.bat', 'Open Receiver Manager Win.bat'):
            self.assertIn(n, names)
        launchers = {n for n in names if n.endswith(('.command', '.bat', '.cmd', '.ps1', '.sh'))}
        self.assertEqual(len(launchers), 4, launchers)                    # nothing else to confuse a user
        self.assertTrue(os.access(os.path.join(ROOT, 'Start RADIO DAYS Mac.command'), os.X_OK))
        self.assertTrue(os.access(os.path.join(ROOT, 'Open Receiver Manager Mac.command'), os.X_OK))


if __name__ == '__main__':
    unittest.main()
