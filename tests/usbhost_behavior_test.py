"""Compile the real usbhost implementation against a temporary sysfs tree."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbHostTest(unittest.TestCase):
    def test_snapshot_and_claim_use_the_same_controller(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            usb = root / 'usb'
            devfs = root / 'dev'
            usb.mkdir()
            for bus, controller in ((5, 'fe900000.dwc3'),
                                    (7, 'fe800000.dwc3'),
                                    (8, 'fe800000.dwc3'),
                                    (9, 'fe800000.dwc3-other')):
                hub = root / controller / 'xhci' / ('usb%d' % bus)
                hub.mkdir(parents=True)
                (usb / hub.name).symlink_to(hub)
            for bus in (5, 7, 9):
                device = usb / ('%d-1' % bus)
                iface = device / (device.name + ':1.0')
                iface.mkdir(parents=True)
                (usb / iface.name).symlink_to(iface)
                for name, value in {'busnum': str(bus), 'devnum': '2',
                                    'idVendor': '05ac', 'idProduct': '12a8',
                                    'speed': '480'}.items():
                    (device / name).write_text(value)
                for name, value in {'bInterfaceClass': 'ff',
                                    'bInterfaceSubClass': 'f0',
                                    'bInterfaceProtocol': '00',
                                    'bInterfaceNumber': '00'}.items():
                    (iface / name).write_text(value)
                for ep in ('ep_81', 'ep_02'):
                    (iface / ep).mkdir()
                    (iface / ep / 'bmAttributes').write_text('02')
                    (iface / ep / 'wMaxPacketSize').write_text('0200')
                (devfs / ('%03d' % bus)).mkdir(parents=True)
                (devfs / ('%03d' % bus) / '002').touch()
            source = (ROOT / 'src/usbhost.c').read_text().replace(
                '"/sys/bus/usb/devices"', '"%s"' % usb).replace(
                '"/dev/bus/usb"', '"%s"' % devfs).replace(
                '"/config/usb_gadget/g1/UDC"', '"%s"' % (root / 'UDC'))
            (root / 'usbhost.c').write_text(source)
            (root / 'test.c').write_text(r'''
#define ioctl test_ioctl
#include "usbhost.c"
int test_ioctl(int fd, unsigned long request, ...) {
    (void)fd; (void)request; return 0;
}
int main(void) {
    struct usbhost host;
    char snapshot[4096];
    setenv("CARPLAY_OTG_UDC", "fe800000.dwc3", 1);
    if (usbhost_role_bus_count() != 2) return 1;
    if (usbhost_open(&host, 0x05ac, 0x12a8) || host.busnum != 7) {
        fprintf(stderr, "FAIL: claim must select the OTG bus, not another Apple device\n"); return 2;
    }
    usbhost_close(&host);
    /* A new usbfs address on the same controller must be claimed again;
     * the matching Apple device on the other controller remains ignored. */
    FILE *devnum = fopen(SYS_USB "/7-1/devnum", "w");
    if (!devnum) return 8;
    fputs("3", devnum); fclose(devnum);
    if (rename(DEV_USB "/007/002", DEV_USB "/007/003")) return 9;
    if (usbhost_open(&host, 0x05ac, 0x12a8) || host.busnum != 7 || host.devnum != 3) {
        fprintf(stderr, "FAIL: re-enumeration must preserve the controller filter\n"); return 10;
    }
    usbhost_close(&host);
    if (usbhost_snapshot(snapshot, sizeof(snapshot)) != 1 ||
        !strstr(snapshot, "7-1 ") || strstr(snapshot, "5-1 ") || strstr(snapshot, "9-1 ")) {
        fprintf(stderr, "FAIL: snapshot counts unrelated USB ports as the head unit\n"); return 3;
    }
    unlink(SYS_USB "/7-1/idVendor");
    if (usbhost_snapshot(snapshot, sizeof(snapshot)) != 0 || snapshot[0]) {
        fprintf(stderr, "FAIL: empty target bus must stay empty despite other USB devices\n"); return 4;
    }
    if (usbhost_open(&host, 0x05ac, 0x12a8) != -1) return 5;
    unlink(SYS_USB "/usb7");
    unlink(SYS_USB "/usb8");
    if (usbhost_role_bus_count() != 0) return 6;
    unsetenv("CARPLAY_OTG_UDC");
    if (usbhost_snapshot(snapshot, sizeof(snapshot)) != 2) return 7;
    return 0;
}
''')
            binary = root / 'test'
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'),
                            str(root / 'test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_role_bus_is_distinguished_from_empty_child_devices(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            usb = root / 'usb'
            controller = root / 'fe800000.dwc3' / 'xhci-hcd.11.auto'
            (controller / 'usb7').mkdir(parents=True)
            (usb).mkdir()
            (usb / 'usb7').symlink_to(controller / 'usb7')
            source = (ROOT / 'src/usbhost.c').read_text().replace(
                '"/sys/bus/usb/devices"', '"%s"' % usb)
            (root / 'usbhost.c').write_text(source)
            (root / 'test.c').write_text(r'''
#include "usbhost.c"
int main(void) {
    setenv("CARPLAY_OTG_UDC", "fe800000.dwc3", 1);
    return usbhost_role_bus_count() == 1 ? 0 : 1;
}
''')
            binary = root / 'test'
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'),
                            str(root / 'test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_endpoint_attributes_and_controller_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            usb = root / 'usb'
            iface = usb / '7-1' / '7-1:1.0'
            for ep in ('ep_81', 'ep_02'):
                d = iface / ep
                d.mkdir(parents=True)
                (d / 'bmAttributes').write_text('02\n')
                (d / 'wMaxPacketSize').write_text('0200\n')
            (iface / 'bInterfaceClass').write_text('ff\n')
            (iface / 'bInterfaceSubClass').write_text('f0\n')
            (iface / 'bInterfaceProtocol').write_text('02\n')
            (iface / 'bInterfaceNumber').write_text('00\n')
            fake_controller = root / 'fe800000.dwc3-wrong' / 'usb7'
            fake_controller.mkdir(parents=True)
            (usb / 'usb7').symlink_to(fake_controller)
            source = (ROOT / 'src/usbhost.c').read_text().replace(
                '"/sys/bus/usb/devices"', '"%s"' % usb)
            (root / 'usbhost.c').write_text(source)
            (root / 'test.c').write_text(r'''
#include "usbhost.c"
int main(void) {
    struct endpoints eps;
    unsigned number = 99;
    int failed = 0;
    if (collect_endpoints("7-1", "7-1:1.0", &eps) || eps.in_max != 512 || eps.out_max != 512) {
        fprintf(stderr, "FAIL: USB endpoint packet sizes are hexadecimal, expected 512\n"); failed++;
    }
    if (match_interface("7-1", "7-1:1.0", &number) == 0) {
        fprintf(stderr, "FAIL: mixed ff/f0/02 is not either supported interface tuple\n"); failed++;
    }
    setenv("CARPLAY_OTG_UDC", "fe800000.dwc3", 1);
    if (bus_is_role_switched(7)) {
        fprintf(stderr, "FAIL: controller substring is not an exact path component\n"); failed++;
    }
    return failed ? 1 : 0;
}
''')
            binary = root / 'test'
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'),
                            str(root / 'test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
