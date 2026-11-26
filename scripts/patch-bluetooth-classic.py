"""Build a narrowly gated compatibility image; never edits its input in place."""
import argparse
import hashlib
from pathlib import Path

BASE_SHA = 'd33810d95ef68eb05cef71144b25bcf4c84117f5b83755d5324c97eb927f1077'
VA = 0x91318
OFFSET = VA - 0x10000
OLD = bytes.fromhex('c80240391f110071e3000054210b00f0c20b00d021b01f9160a08052426c23919558fe97')

def patch(original, guard):
    if hashlib.sha256(original).hexdigest() != BASE_SHA:
        raise ValueError('Unsupported Bluetooth firmware: original SHA256 differs')
    if len(guard) != len(OLD) or original[OFFSET:OFFSET+len(OLD)] != OLD:
        raise ValueError('Guard length or original instructions differ')
    # Verify the control-flow targets independently of the assembler.
    import struct
    words=struct.unpack('<9I',guard)
    def sx(n,bits): return n-(1<<bits) if n>>(bits-1) else n
    if words[0]!=0xd101c3a0 or words[1]!=0x52800021 or words[3]!=0x52800053:
        raise ValueError('Guard register setup differs')
    if (words[2]&0xfc000000)!=0x94000000 or VA+8+4*sx(words[2]&0x3ffffff,26)!=0x10fbf4:
        raise ValueError('Wrong ACL query branch')
    if (words[4]&0xff00001f)!=0x35000000 or VA+16+4*sx((words[4]>>5)&0x7ffff,19)!=0x9136c:
        raise ValueError('Wrong bonded continuation')
    if any(x!=0xd503201f for x in words[5:]):raise ValueError('Unexpected padding')
    result=bytearray(original);result[OFFSET:OFFSET+len(OLD)]=guard
    return bytes(result)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('original');p.add_argument('guard');p.add_argument('output');a=p.parse_args()
    source=Path(a.original);dest=Path(a.output)
    if source.resolve()==dest.resolve():raise SystemExit('Input must remain intact')
    result=patch(source.read_bytes(),Path(a.guard).read_bytes());dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(result)
    print('SHA256='+hashlib.sha256(result).hexdigest())
