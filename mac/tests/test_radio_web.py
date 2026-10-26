import os
import re
import sys
import tempfile
import unittest

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import receiver_manager as rm


class Store:
    def __init__(self, root):
        self.path = os.path.join(root, 'servers.json')
        self.servers = [
            {'name': 'One', 'short': 'ONE', 'host': 'one.example', 'port': 8073, 'password': '', 'lat': 0, 'lon': 0},
            {'name': 'Two', 'short': 'TWO', 'host': 'two.example', 'port': 8073, 'password': '', 'lat': 1, 'lon': 1},
        ]
    def load(self): return [dict(x) for x in self.servers]
    def info(self): return {'mtime': None}


class Engine:
    def __init__(self):
        self.freq, self.mode, self.volume, self.muted = 7100.0, 'am', 70, False
        self.scan, self.output, self.server_index = 'off', 'mac', 0
        self.servers = Store('/tmp').servers
    def state(self):
        return {'freq': self.freq, 'mode': self.mode, 'volume': self.volume, 'muted': self.muted,
                'scan': self.scan, 'output': self.output, 'link': 'connected', 's': 5,
                'server': self.servers[self.server_index]['short'], 'server_index': self.server_index}
    def set_freq(self, value): self.freq = float(value)
    def set_mode(self, value): self.mode = value
    def set_volume(self, value): self.volume = max(0, min(100, int(value)))
    def set_muted(self, value): self.muted = bool(value)
    def start_scan(self, direction, step): self.scan = 'up' if direction > 0 else 'down'
    def stop_scan(self): self.scan = 'off'
    def switch_server(self, index): self.server_index = index
    def set_output(self, value): self.output = value


class RadioWebTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.engine = Engine()
        self.manager = rm.Manager(Store(self.temp.name), self.engine)
    def tearDown(self): self.temp.cleanup()
    def test_live_state_contains_engine_and_receiver_list(self):
        state = self.manager.radio_state()
        self.assertTrue(state['running'])
        self.assertEqual((state['freq'], state['server'], len(state['servers'])), (7100.0, 'ONE', 2))
        self.assertEqual((state['servers'][0]['lat'], state['servers'][0]['lon'], state['servers'][0]['has_position']), (0, 0, True))
    def test_browser_actions_use_engine(self):
        self.manager.radio_action({'action': 'step', 'value': 1, 'step': 5})
        self.manager.radio_action({'action': 'mode', 'value': 'usb'})
        self.manager.radio_action({'action': 'mute'})
        self.manager.radio_action({'action': 'server-delta', 'value': 1})
        self.assertEqual((self.engine.freq, self.engine.mode, self.engine.muted, self.engine.server_index), (7105.0, 'usb', True, 1))
        self.manager.radio_action({'action': 'mute'})
        self.assertFalse(self.engine.muted)
    def test_browser_output_button_toggles_both_ways(self):
        first = self.manager.radio_action({'action': 'output'})
        self.assertEqual((self.engine.output, first['output']), ('cardputer', 'cardputer'))
        second = self.manager.radio_action({'action': 'output'})
        self.assertEqual((self.engine.output, second['output']), ('mac', 'mac'))
    def test_guide_without_radio_reports_not_running(self):
        state = rm.Manager(Store(self.temp.name), None).radio_state()
        self.assertFalse(state['running'])
    def test_every_script_and_stylesheet_of_the_web_controller_is_served(self):
        # A page asset missing from STATIC answers 404, so its controls silently do nothing (the X button did).
        with open(os.path.join(MAC, 'manager', 'radio', 'index.html'), encoding='utf-8') as f:
            html = f.read()
        refs = re.findall(r'(?:src|href)="(/radio/[^"]+)"', html)
        self.assertIn('/radio/wifi-toggle.js', refs)
        for ref in refs:
            self.assertIn(ref, rm.STATIC)
            self.assertTrue(os.path.isfile(os.path.join(MAC, 'manager', rm.STATIC[ref][0])), ref)
    def test_wifi_toggle_is_forwarded_to_the_cardputer(self):
        self.engine.request_card_command = lambda command: setattr(self.engine, 'card_command', command)
        self.manager.radio_action({'action': 'wifi-toggle'})
        self.assertEqual(self.engine.card_command, 'wifi-toggle')


if __name__ == '__main__': unittest.main()
