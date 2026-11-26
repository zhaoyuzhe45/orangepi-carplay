"""Exercise the assembled ARM64 guard, including both runtime connection states."""
import sys,importlib.util
from pathlib import Path
root=Path(__file__).resolve().parents[1]
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
spec=importlib.util.spec_from_file_location('patcher',root/'scripts/patch-bluetooth-classic.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
guard=(root/'build/bluetooth-classic/guard.bin').read_bytes()
original=(root/'build/deps/bluetooth/bluetooth.default.arm64.so').read_bytes()
modified=m.patch(original,guard)
assert len(modified)==len(original)
assert modified[:m.OFFSET]==original[:m.OFFSET]
assert modified[m.OFFSET+len(guard):]==original[m.OFFSET+len(guard):]
for active in [0,1]:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x90000,0x3000);u.mem_map(0x10f000,0x2000)
 u.mem_write(m.VA,guard);u.reg_write(UC_ARM64_REG_X29,0x300000)
 u.reg_write(UC_ARM64_REG_X20,0xa123);u.reg_write(UC_ARM64_REG_X21,0xb123);u.reg_write(UC_ARM64_REG_X22,0xc123)
 visited=[]
 def hook(uc,addr,size,data):
  visited.append(addr)
  if addr==0x10fbf4:
   assert uc.reg_read(UC_ARM64_REG_X0)==0x300000-112
   assert uc.reg_read(UC_ARM64_REG_W1)==1
   uc.reg_write(UC_ARM64_REG_W0,active);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
  elif addr in (0x9133c,0x9136c):uc.emu_stop()
 u.hook_add(UC_HOOK_CODE,hook);u.emu_start(m.VA,0,count=40)
 assert visited[-1]==(0x9136c if active else 0x9133c),visited
 assert visited.count(0x10fbf4)==1
 assert u.reg_read(UC_ARM64_REG_W19)==2
 assert u.reg_read(UC_ARM64_REG_X20)==0xa123 and u.reg_read(UC_ARM64_REG_X21)==0xb123 and u.reg_read(UC_ARM64_REG_X22)==0xc123
for raw,g in [(original[:-1],guard),(original,b'\0'*36),(original,guard[:-1])]:
 try:m.patch(raw,g)
 except ValueError:pass
 else:raise AssertionError('Unsupported input accepted')
print('PASS: ARM64 active-Classic skip, LE-only fallthrough, register preservation, exact change scope, rejected incompatible inputs')
