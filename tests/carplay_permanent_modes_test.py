"""Independent binary plist inputs exercise the real resource implementation."""
from pathlib import Path
import plistlib, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
DRIVER=r'''
#include "carplay_modes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
 struct carplay_modes s;carplay_modes_init(&s,atoi(argv[1]));
 for(int i=2;i<argc;i++) {
  int result;
  if(!strcmp(argv[i],"reclaim")) result=carplay_modes_reclaim_unowned(&s);
  else if(!strcmp(argv[i],"invalid")) {
   s.constraints[0][0]=999;struct carplay_modes before=s;
   if(carplay_modes_reclaim_unowned(&s)!=-1 || memcmp(&s,&before,sizeof(s)))return 3;
   continue;
  } else {
  unsigned char body[4096];FILE *f=fopen(argv[i],"rb");if(!f)return 2;
  size_t n=fread(body,1,sizeof(body),f);fclose(f);
  result=carplay_modes_apply(&s,body,n);
  }
  printf("%d %d %d %d %d %d %d\n",result,s.screen_entity,s.screen_permanent_entity,
    s.audio_entity,s.audio_permanent_entity,s.screen_borrowed,s.audio_borrowed);
 }
 return 0;
}
'''
def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp=Path(tmp);(tmp/'driver.c').write_text(DRIVER)
        exe=tmp/'test'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(tmp/'driver.c'),str(ROOT/'src/carplay_modes.c'),str(ROOT/'src/airplay_bplist.c'),'-o',str(exe)],check=True)
        def run(audio,*resources):
            paths=[]
            for i,value in enumerate(resources):
                if isinstance(value,str):paths.append(value);continue
                p=tmp/f'{i}.plist';p.write_bytes(plistlib.dumps({'type':'changeModes','params':{'resources':value}},fmt=plistlib.FMT_BINARY));paths.append(str(p))
            return [tuple(map(int,line.split())) for line in subprocess.check_output([str(exe),str(audio),*paths],text=True).splitlines()]
        def take(id=1,**kwargs):return dict(resourceID=id,transferType=1,transferPriority=500,takeConstraint=100,borrowConstraint=100,**kwargs)
        untake={'resourceID':1,'transferType':2}
        both=run(1,[take()],[untake],[untake])
        assert both==[(0,2,2,1,1,0,0),(0,0,0,1,1,0,0),(0,0,0,1,1,0,0)],both
        # A late release cannot discard the controller's permanent ownership.
        assert run(1,[untake])==[(0,1,1,1,1,0,0)]
        # Borrow upgraded to Take no longer resumes the previous permanent owner.
        borrowed={'resourceID':1,'transferType':3}
        assert run(1,[borrowed],[take()],[untake])[-1]==(0,0,0,1,1,0,0)
        bad=take();bad['takeConstraint']=999
        assert run(1,[{'resourceID':2,'transferType':3},bad])==[(-1,1,1,1,1,0,0)]
        for key in ['transferPriority','takeConstraint','borrowConstraint']:
            bad=take();del bad[key];assert run(1,[bad])[0][0]==-1
            bad=take();bad[key]=True;assert run(1,[bad])[0][0]==-1
        for key in ['transferPriority','takeConstraint','borrowConstraint','unborrowConstraint']:
            bad=dict(untake);bad[key]=100;assert run(1,[take()],[bad])[-1]==(-1,2,2,1,1,0,0)
        assert run(0,[take(2)],[{'resourceID':2,'transferType':2}])[-1]==(0,1,1,0,0,0,0)
        assert run(1,[take()],'reclaim')[-1]==(0,2,2,1,1,0,0)
        assert run(1,[take()],[untake],'reclaim','reclaim')[-2:]==[(1,1,1,1,1,0,0),(0,1,1,1,1,0,0)]
        assert run(1,[take(),take(2)],[untake,{'resourceID':2,'transferType':2}],'reclaim')[-1]==(3,1,1,1,1,0,0)
        assert run(0,[take(),take(2)],[untake,{'resourceID':2,'transferType':2}],'reclaim')[-1]==(1,1,1,0,0,0,0)
        assert run(1,[take()],[untake],[borrowed],'reclaim')[-1]==(0,2,0,1,1,1,0)
        run(1,'invalid')
        print('Permanent Take/Untake/validation/atomicity: PASS')
if __name__=='__main__':main()
