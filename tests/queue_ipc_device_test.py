"""Queue-only root IPC verification using the explicit test player; never sends phone controls."""
import subprocess,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ADB=[str(ROOT/'tools/scrcpy/adb.exe'),'-P','5038','-s','192.168.2.67:5555']
def shell(*a):
 r=subprocess.run(ADB+['shell',*a],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=12)
 if r.returncode:raise AssertionError(r.stdout+r.stderr)
 return r.stdout.strip()
def ipc(op=0,index=0,version=None):
 a=['su','0','env','CLASSPATH=/data/local/tmp/queue-probes.dex','app_process','-Xmx64m','/system/bin','AppQueueIpcProbe',str(op),str(index)]
 if version is not None:a.append(str(version))
 return bytes.fromhex(shell(*a))
def snapshot():
 b=ipc();assert b[:4]==b'CPN3',b[:4];off=24;fields=[]
 for _ in range(6):
  n=struct.unpack_from('>H',b,off)[0];off+=2;fields.append(b[off:off+n].decode());off+=n
 assert fields[5]=='com.carplaybox.appfixture',fields
 return fields,struct.unpack_from('>IIIIqB',b,off)
def wait_title(title):
 for _ in range(20):
  data=snapshot()
  if data[0][0]==title:return data
  time.sleep(.2)
 raise AssertionError((title,data))
def page(start):
 b=ipc(9,start);assert b[:4]==b'CQL1';sampled,v,total,offset,count=struct.unpack_from('>QqIIH',b,4);assert offset==start
 entries=[];pos=30
 for _ in range(count):
  ident=struct.unpack_from('>q',b,pos)[0];pos+=8;fields=[]
  for _ in range(3):
   n=struct.unpack_from('>H',b,pos)[0];pos+=2;fields.append(b[pos:pos+n].decode());pos+=n
  entries.append((ident,fields))
 assert pos==len(b)
 return v,total,entries
assert shell('getprop','ro.serialno')=='36QW5PRVUE'
snapshot();v,total,entries=page(238);assert total==240 and len(entries)==2 and entries[1][0]==24240,(total,entries)
assert ipc(10,239,v)==b'\0';wait_title('Fixture歌曲240')
shell('am','start-foreground-service','-n','com.carplaybox.appfixture/.AppMediaFixture','--ez','reverse','true')
for _ in range(25):
 fresh=page(0)
 if fresh[0]!=v:break
 time.sleep(.2)
assert fresh[0]!=v and ipc(10,0,v)==b'\1','stale selection accepted'
assert ipc(10,240,fresh[0])==b'\1','out-of-range selection accepted'
assert ipc(10,239,fresh[0])==b'\0';wait_title('Fixture歌曲1')
print('PASS CPN3/CQL1 root IPC, paginated 240-item queue, non-index ID 24240, stale/out-of-range rejection, reverse-order selection')
