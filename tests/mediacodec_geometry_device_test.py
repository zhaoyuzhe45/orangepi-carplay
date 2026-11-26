#!/usr/bin/env python3
"""Compare old/new real encoder geometry; always restore original wm size."""
from pathlib import Path
import argparse
import json
import re
import subprocess
import time
import numpy as np
from PIL import Image

root = Path(__file__).resolve().parents[1]
out = root / 'build/mediacodec-20261110'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device', required=True)
parser.add_argument('--adb', default='adb')
args = parser.parse_args()
adb = [args.adb, '-s', args.device]
out.mkdir(parents=True, exist_ok=True)
def run(*args):
    return subprocess.check_output([*adb, *args], text=True, stderr=subprocess.STDOUT)
original = run('shell', 'wm', 'size')
override = re.search(r'Override size: (\d+x\d+)', original)
restore = override.group(1) if override else 'reset'
records = []
try:
    for size in ('1920x900', '1800x1080', '1920x1080'):
        print('CHECK', size, flush=True)
        run('shell', 'su', '0', 'wm', 'size', size)
        # wm size recreates the launcher's surface. Wait for that transition;
        # its first captured buffer may be black even on the old encoder.
        time.sleep(5)
        run('shell','su','0','screenrecord','--size','1920x1080','--bit-rate',
            '8000000','--time-limit','2','--output-format=h264','/data/local/tmp/old-encoder-probe.h264')
        probe=run('shell','su','0','/data/local/tmp/encoder-probe','5','1')
        images=[]
        for label,remote in [('old','old-encoder-probe.h264'),('new','mediacodec-probe.h264')]:
            run('shell','su','0','chmod','644','/data/local/tmp/'+remote)
            path=out/(label+'-'+size+'.h264')
            run('pull','/data/local/tmp/'+remote,str(path))
            png=path.with_suffix('.png')
            subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(path),'-vf','reverse','-frames:v','1',str(png)],check=True)
            images.append(np.asarray(Image.open(png).convert('RGB'),dtype=float))
        a,b=images
        assert a.shape==b.shape==(1080,1920,3)
        width,height=map(int,size.split('x'))
        # Ignore bottom clock and small icon animation: compare textured grids.
        roi=(slice(50,min(height-80,850)),slice(60,min(width-60,1750)))
        aa,bb=a[roi],b[roi]
        scores={}
        for dx,dy in [(0,0),(1,0),(-1,0),(0,1),(0,-1),(60,0),(0,90)]:
            h,w=aa.shape[:2]; ax=max(dx,0); bx=max(-dx,0); ay=max(dy,0); by=max(-dy,0)
            scores[f'{dx},{dy}']=float(np.abs(aa[ay:h-by,ax:w-bx]-bb[by:h-ay,bx:w-ax]).mean())
        assert min(scores,key=scores.get)=='0,0', scores
        assert scores['0,0']<2.0, scores
        record={'logical':size,'capture':'1920x1080','zero_shift_mae':scores['0,0'],
                'shift_scores':scores,'probe':probe.strip()}
        records.append(record)
        print(json.dumps(record),flush=True)
finally:
    run('shell','su','0','wm','size',restore)
    final=run('shell','wm','size')
    assert final.strip()==original.strip(), (original,final)
    (out/'geometry-results.json').write_text(json.dumps({'original':original,'restored':final,'cases':records},indent=2))
print('PASS: old/new geometry matches at zero displacement; original wm size restored')
