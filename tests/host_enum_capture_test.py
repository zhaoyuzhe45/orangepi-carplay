"""Run the real shell diagnostic against a changing, synthetic USB topology."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/diagnostics/capture-usbhost-enum.sh'


class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in ('bus', 'mon', 'udc', 'out'):
            (self.root / name).mkdir()
        (self.root / 'mode').write_text('host\n')
        (self.root / 'udc/state').write_text('not attached\n')
        self.env = dict(os.environ, USB_BUS=str(self.root / 'bus'),
                        MON_ROOT=str(self.root / 'mon'), MODE_PATH=str(self.root / 'mode'),
                        UDC_STATE=str(self.root / 'udc/state'), OUT=str(self.root / 'out'),
                        TRACE_ROOT=str(self.root / 'no-tracing'), CARPLAY_OTG_UDC='fe800000.dwc3')
        self.hub(5, 'fe900000.dwc3')

    def hub(self, bus, controller):
        path = self.root / 'devices' / controller / 'xhci' / ('usb%d' % bus)
        path.mkdir(parents=True)
        (self.root / 'bus' / ('usb%d' % bus)).symlink_to(path)
        (self.root / 'mon' / ('%du' % bus)).write_text('bus%d sample\n' % bus)

    def run_script(self, *args):
        try:
            return subprocess.run(['sh', str(SCRIPT), *args], env=self.env,
                                  capture_output=True, text=True, timeout=8)
        except subprocess.TimeoutExpired:
            self.fail('diagnostic ignored the supplied topology or bounded wait')

    def test_bus_five_is_not_assumed_to_be_the_otg_port(self):
        self.hub(7, 'fe800000.dwc3')
        self.hub(8, 'fe800000.dwc3')
        result = self.run_script('--buses')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), ['7', '8'])

    def test_no_target_does_not_capture_the_other_controller(self):
        result = self.run_script('1', '1')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('no host bus for fe800000.dwc3', result.stdout)
        self.assertFalse(list((self.root / 'out').glob('*/usbmon-5.txt')))

    def test_host_capture_uses_target_and_keeps_separate_runs(self):
        self.hub(7, 'fe800000.dwc3')
        for _ in range(2):
            result = self.run_script('1', '1')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runs = list((self.root / 'out').glob('usbhost-*'))
        self.assertEqual(len(runs), 2)
        for run in runs:
            self.assertEqual((run / 'usbmon-7.txt').read_text(), 'bus7 sample\n')
            self.assertFalse((run / 'usbmon-5.txt').exists())
            self.assertTrue((run / 'dmesg-before.txt').exists())
            self.assertTrue((run / 'dmesg-after.txt').exists())

    def test_peripheral_mode_does_not_start_host_capture(self):
        self.hub(7, 'fe800000.dwc3')
        (self.root / 'mode').write_text('peripheral\n')
        result = self.run_script('1', '1')
        self.assertEqual(result.returncode, 1)
        self.assertFalse(list((self.root / 'out').glob('*/usbmon-7.txt')))


if __name__ == '__main__':
    unittest.main()
