"""Real usbhost bulk code with deterministic USBDEVFS ioctl outcomes."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StallTest(unittest.TestCase):
    def test_bounded_read_clear_halt_and_no_output_replay(self):
        source = Path(os.environ.get('USBHOST_SOURCE', ROOT / 'src/usbhost.c')).read_text()
        code = r'''
#define ioctl test_ioctl
#include "usbhost.c"
static int mode,bulks,clears,bad;
int test_ioctl(int fd,unsigned long op,...) {
    va_list args;va_start(args,op);void *p=va_arg(args,void*);va_end(args);
    if(fd!=19) bad=1;
    if(op==USBDEVFS_CLEAR_HALT) {
        clears++;
        if(*(unsigned int*)p!=(mode>=4 ? 2u : 0x81u)) bad=1;
        if(mode==3) {errno=ENODEV;return -1;}
        return 0;
    }
    if(op!=USBDEVFS_BULK) {bad=1;return -1;}
    struct usbdevfs_bulktransfer *b=p;bulks++;
    if(mode==1 && bulks==2) {memset(b->data,7,3);return 3;}
    if(mode==5) return 2;
    if(mode==6) {errno=ETIMEDOUT;return -1;}
    errno=EPIPE;return -1;
}
int main(void) {
    struct usbhost h={.fd=19,.ep_in=0x81,.ep_out=2};char buf[8]={0};
    for(mode=1;mode<=6;mode++) {
        bulks=clears=bad=0;
        int n=mode>=4 ? usbhost_write(&h,buf,sizeof(buf),100) : usbhost_read(&h,buf,sizeof(buf),100);
        int e=errno;
        if(mode==1 && (n!=3 || bulks!=2 || clears!=1 || buf[0]!=7)) bad=1;
        if(mode==2 && (n!=-1 || e!=EPIPE || bulks!=3 || clears!=2)) bad=1;
        if(mode==3 && (n!=-1 || e!=ENODEV || bulks!=1 || clears!=1)) bad=1;
        if(mode==4 && (n!=-1 || e!=EPIPE || bulks!=1 || clears!=1)) bad=1;
        if(mode==5 && (n!=2 || bulks!=1 || clears)) bad=1;
        if(mode==6 && (n!=-1 || e!=ETIMEDOUT || bulks!=1 || clears)) bad=1;
        if(bad) {fprintf(stderr,"FAIL mode %d n=%d errno=%d bulk=%d clear=%d\n",mode,n,e,bulks,clears);return 1;}
    }
    puts("usbhost stall: bounded input recovery and no uncertain output replay passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'usbhost.c').write_text(source)
            c = root / 'test.c'; c.write_text(code)
            binary = root / 'test'
            subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'src'), str(c), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stdout, end='')


if __name__ == '__main__': unittest.main()
