"""Real Android MediaSession integration; requires the test-only AppFixture APK."""
from pathlib import Path
import subprocess,time
ROOT=Path(__file__).resolve().parents[1]
ADB=str(ROOT/'tools/scrcpy/adb.exe');DEVICE='192.168.2.67:5555'
def shell(*args):
    result=subprocess.run([ADB,'-s',DEVICE,'shell',*args],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=15)
    if result.returncode:raise AssertionError(result.stdout+result.stderr)
    return result.stdout.strip()
def read():return shell('/data/local/tmp/carplay-app-probe')
def command(op,arg):
    result=shell('/data/local/tmp/carplay-app-probe',str(op),str(arg));print(result);assert 'result=0' in result
def await_state(*parts):
    for _ in range(12):
        text=read()
        if all(part in text for part in parts):print(text);return
        time.sleep(.3)
    raise AssertionError((parts,text))
assert shell('getprop','ro.serialno')=='36QW5PRVUE'
await_state('package=com.carplaybox.appfixture','title=Fixture歌曲1','status=1','seek=1')
command(1,127);await_state('status=2')
command(3,98765);await_state('status=2 position=98765')
command(1,87);await_state('title=Fixture歌曲2')
command(1,88);await_state('title=Fixture歌曲1')
command(1,126);await_state('status=1')
command(1,85);await_state('status=2')
command(2,1);command(2,2);time.sleep(.5);command(2,2);command(2,3)
time.sleep(.5)
evidence=shell('cat','/data/user/0/com.carplaybox.appfixture/shared_prefs/evidence.xml')
assert 'name="non_root_rejected" value="true"' in evidence,evidence
assert 'name="voice_launches" value="1"' in evidence,evidence
print(evidence)
shell('am','force-stop','com.carplaybox.appfixture');await_state('status=0','package=\ntitle=')
print('PASS real metadata, pause/play/toggle, next/previous, seek, root-only IPC, voice edge and session cleanup')
