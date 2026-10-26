import json
import os
import sys
import unittest

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


if __name__ == '__main__':
    unittest.main()
