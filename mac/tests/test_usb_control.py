import contextlib
import io
import json
import os
import sys
import time
import unittest
from types import SimpleNamespace

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import radiodays_sdr as radio


class UsbControlTests(unittest.TestCase):
    def setUp(self):
        servers = [{'name': 'Test', 'short': 'TEST', 'host': '127.0.0.1', 'port': 9,
                    'password': '', 'lat': None, 'lon': None}]
        self.engine = radio.Engine(servers, radio.NullSink(), fake_source=True)
        self.usb = radio.SerialController(self.engine, 'secret')

    def response(self, line):
        answer = self.usb.handle_line(line)
        self.assertIsNotNone(answer)
        return json.loads(answer.split(' ', 2)[2])

    def test_state_request_uses_same_engine_state_as_http(self):
        state = self.response('RDSDR_REQ 17 secret /state')
        self.assertEqual((state['freq'], state['output'], state['server']), (10000.0, 'mac', 'TEST'))

    def test_usb_request_changes_output(self):
        state = self.response('RDSDR_REQ 18 secret /output?value=cardputer')
        self.assertEqual((state['output'], self.engine.output), ('cardputer', 'cardputer'))

    def test_unrelated_logs_are_ignored_and_token_is_checked(self):
        self.assertIsNone(self.usb.handle_line('[BOOT] RADIO DAYS SDR'))
        self.assertEqual(self.response('RDSDR_REQ 19 wrong /state')['error'], 'unauthorized')

    def test_request_is_recovered_if_a_diagnostic_prefix_is_adjacent(self):
        state = self.response('[AUDIO] status RDSDR_REQ 20 secret /state')
        self.assertEqual(state['server'], 'TEST')

    def test_cardputer_wifi_state_is_reported_to_the_web_state(self):
        state = self.response('RDSDR_REQ 21 secret /state?card_wifi=0')
        self.assertFalse(state['cardputer_wifi'])
        self.assertTrue(state['cardputer_pc'])

    def test_web_wifi_command_is_included_in_usb_state(self):
        self.engine.request_card_command('wifi')
        state = self.response('RDSDR_REQ 22 secret /state?card_wifi=0')
        self.assertEqual(state['card_command'], 'wifi')
        self.assertGreater(state['card_command_seq'], 0)


class FakeSerialError(Exception):
    pass


class FakePort:
    """Records how the port is configured and opened; readline() plays a script (bytes, or an exception to raise)."""
    script = []
    fail_open = None

    def __init__(self, port=None, **settings):
        object.__setattr__(self, 'settings', settings)
        object.__setattr__(self, 'events', [])
        object.__setattr__(self, 'written', b'')
        object.__setattr__(self, 'closed', False)
        object.__setattr__(self, 'lines', list(FakePort.script))

    def __setattr__(self, name, value):
        self.events.append((name, value))
        object.__setattr__(self, name, value)

    def open(self):
        if FakePort.fail_open:
            raise FakePort.fail_open
        self.events.append(('open', None))

    def reset_input_buffer(self):
        pass

    def readline(self):
        if not self.lines:
            return b''
        item = self.lines.pop(0)
        if isinstance(item, Exception):
            raise item
        return item

    def write(self, data):
        object.__setattr__(self, 'written', self.written + data)

    def flush(self):
        pass

    def close(self):
        object.__setattr__(self, 'closed', True)


class UsbPortHandlingTests(unittest.TestCase):
    def setUp(self):
        servers = [{'name': 'Test', 'short': 'TEST', 'host': '127.0.0.1', 'port': 9,
                    'password': '', 'lat': None, 'lon': None}]
        self.engine = radio.Engine(servers, radio.NullSink(), fake_source=True)
        self.opened = []

        def make_port(*args, **kwargs):
            port = FakePort(*args, **kwargs)
            self.opened.append(port)
            return port
        self.serial = SimpleNamespace(Serial=make_port, SerialException=FakeSerialError)
        FakePort.script, FakePort.fail_open = [], None

    def controller(self, windows):
        usb = radio.SerialController(self.engine, 'secret', windows=windows)
        usb.SILENT_S = 0.05
        return usb

    def serve(self, usb, device):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            answered = usb.serve(self.serial, device)
        return answered, out.getvalue()

    def test_windows_open_raises_dtr_and_clears_rts_without_handshake(self):
        port = radio.open_cardputer_port(self.serial, 'COM4', windows=True)
        self.assertFalse(port.settings['dsrdtr'])            # no DTR handshake, no DSR output flow control
        names = [name for name, _ in port.events]
        before_open = port.events[:names.index('open')]
        self.assertIn(('dtr', True), before_open)             # applied in the port settings at open: no edge
        self.assertIn(('rts', False), before_open)
        self.assertEqual(port.events[names.index('open') + 1:], [('dtr', True), ('rts', False)])
        self.assertNotIn(('rts', True), port.events)          # never part of the ESP32 bootloader sequence
        self.assertNotIn(('dtr', False), port.events)

    def test_mac_open_is_unchanged(self):
        port = radio.open_cardputer_port(self.serial, '/dev/cu.usbmodem1101', windows=False)
        self.assertTrue(port.settings['dsrdtr'])
        self.assertEqual(port.events, [('rts', False), ('port', '/dev/cu.usbmodem1101'), ('open', None)])

    def test_answering_port_becomes_the_preferred_one(self):
        usb = self.controller(windows=True)
        FakePort.script = [b'[BOOT] log line\n', b'RDSDR_REQ 7 secret /state\n']
        answered, out = self.serve(usb, 'COM4')
        self.assertTrue(answered)
        self.assertEqual(usb.proven, 'COM4')
        self.assertIn(b'RDSDR_RES 7 ', self.opened[0].written)
        self.assertTrue(self.opened[0].closed)
        self.assertIn('active on COM4', out)
        self.assertEqual(usb.ordered(['COM3', 'COM4'], now=0.0), ['COM4', 'COM3'])

    def test_silent_port_is_not_retried_while_another_candidate_exists(self):
        usb = self.controller(windows=True)
        answered, out = self.serve(usb, 'COM3')
        self.assertFalse(answered)
        self.assertIn('no RADIO DAYS request on COM3', out)
        now = time.monotonic()
        self.assertEqual(usb.ordered(['COM3', 'COM4'], now), ['COM4'])
        self.assertEqual(usb.ordered(['COM3'], now), ['COM3'])               # the only candidate is always tried
        self.assertEqual(usb.ordered(['COM3', 'COM4'], now + 60), ['COM3', 'COM4'])
        answered, out = self.serve(usb, 'COM3')
        self.assertEqual(out, '')                                           # the same problem is printed once

    def test_clearcommerror_closes_the_port_and_keeps_the_preference(self):
        usb = self.controller(windows=True)
        FakePort.script = [b'RDSDR_REQ 8 secret /state\n',
                           FakeSerialError("ClearCommError failed (PermissionError(13, '', None, 22))")]
        answered, out = self.serve(usb, 'COM4')
        self.assertTrue(answered)
        self.assertTrue(self.opened[0].closed)
        self.assertIsNone(usb.port)
        self.assertIn('disconnected from COM4: ClearCommError failed', out)
        self.assertEqual(usb.ordered(['COM3', 'COM4'], time.monotonic()), ['COM4', 'COM3'])

    def test_port_that_cannot_be_opened_waits(self):
        usb = self.controller(windows=True)
        FakePort.fail_open = PermissionError(13, 'Access is denied')
        answered, out = self.serve(usb, 'COM3')
        self.assertFalse(answered)
        self.assertIn('cannot open COM3', out)
        self.assertEqual(usb.ordered(['COM3', 'COM4'], time.monotonic()), ['COM4'])


if __name__ == '__main__':
    unittest.main()
