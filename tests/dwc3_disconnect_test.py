"""Run SDK production UDC-stop with hardware/IRQ boundaries simulated.

Pass DWC3_GADGET_SOURCE to test a different source snapshot. No target MMIO.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from iap2d_host_runtime_test import function

SOURCE = Path(os.environ.get('DWC3_GADGET_SOURCE',
    str(Path(__file__).resolve().parents[1] / 'system-patch/kernel/gadget.c')))
if not SOURCE.is_file():
    raise SystemExit('Set DWC3_GADGET_SOURCE to the patched SDK kernel/drivers/usb/dwc3/gadget.c')


class DisconnectTest(unittest.TestCase):
    def test_stop_clears_only_quiesced_connection_after_irq_drain(self):
        body = function(SOURCE.read_text(), 'static int dwc3_gadget_stop(')
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#define DWC3_DCTL 0
#define DWC3_DSTS 1
#define DWC3_DCTL_RUN_STOP (1u << 31)
#define DWC3_DSTS_DEVCTRLHLT (1u << 22)
typedef uint32_t u32;
struct usb_gadget { int unused; };
struct dwc3 {
    bool connected, pullups_connected;
    void *gadget_driver;
    int lock, irq_gadget;
    void *ev_buf, *dev;
    u32 regs[2];
};
static struct dwc3 controller;
static bool suspended, irq_drained, late_reset, bad_read, stopped;
#define gadget_to_dwc(g) ((void)(g), &controller)
#define spin_lock_irqsave(l,f) ((void)(l), (f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l), (void)(f))
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_dbg(...) ((void)0)
static int pm_runtime_suspended(void *d) { (void)d; return suspended; }
static u32 dwc3_readl(u32 *regs, int reg) {
    if (suspended) bad_read=true;
    return regs[reg];
}
static void __dwc3_gadget_stop(struct dwc3 *d) { (void)d; stopped=true; }
static void free_irq(int irq, void *buf) {
    (void)irq; (void)buf;
    if (late_reset) controller.connected=true;
    irq_drained=true;
}
''' + body + r'''
int main(void) {
    /* halted; late IRQ; running; stop timed out; pullups still on; suspended */
    const unsigned cases[][5] = {
        {0,1,0,0,0}, {0,1,0,0,1}, {1,0,0,0,0},
        {0,0,0,0,0}, {0,1,1,0,0}, {0,0,0,1,1}
    };
    struct usb_gadget g;
    for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); i++) {
        controller=(struct dwc3){.connected=true, .gadget_driver=&g};
        controller.regs[DWC3_DCTL]=cases[i][0] ? DWC3_DCTL_RUN_STOP : 0;
        controller.regs[DWC3_DSTS]=cases[i][1] ? DWC3_DSTS_DEVCTRLHLT : 0;
        controller.pullups_connected=cases[i][2];
        suspended=cases[i][3]; late_reset=cases[i][4];
        stopped=irq_drained=bad_read=false;
        bool expect_connected=!(suspended ||
            (!cases[i][0] && cases[i][1] && !cases[i][2]));
        if (dwc3_gadget_stop(&g) || controller.connected!=expect_connected ||
            controller.gadget_driver || !stopped || !irq_drained || bad_read) {
            fprintf(stderr,"case %u: connected=%d expected=%d irq_drained=%d bad_read=%d\n",
                i,controller.connected,expect_connected,irq_drained,bad_read);
            return 1;
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp)/'test.c'; c.write_text(harness)
            binary = Path(tmp)/'test'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(c),'-o',str(binary)],check=True)
            r = subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)


if __name__ == '__main__':
    unittest.main()
