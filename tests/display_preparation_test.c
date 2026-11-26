#include "carplay_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void iap2_log(const char *fmt, ...) { (void)fmt; }
int main(void)
{
    const char *ok = "version=1\nboot_id=boot-a\nsequence=2\nrequest_id=cp-1\nphase=complete\nmode=1280x720@60\ndensity=240\nresult=0\ndetail=verified\n";
    assert(carplay_display_parse_preparation_result(ok,"boot-a","cp-1",1280,720,60)==1);
    assert(carplay_display_parse_preparation_result(ok,"boot-b","cp-1",1280,720,60)==0);
    assert(carplay_display_parse_preparation_result(ok,"boot-a","cp-2",1280,720,60)==0);
    assert(carplay_display_parse_preparation_result(ok,"boot-a","cp-1",800,480,60)==0);
    const char *pending = "version=1\nboot_id=boot-a\nrequest_id=cp-1\nphase=applying\nmode=1280x720@60\nresult=0\n";
    assert(carplay_display_parse_preparation_result(pending,"boot-a","cp-1",1280,720,60)==0);
    const char *failed = "version=1\nboot_id=boot-a\nrequest_id=cp-1\nphase=failed\nmode=1280x720@60\nresult=70\n";
    assert(carplay_display_parse_preparation_result(failed,"boot-a","cp-1",1280,720,60)==-1);
    const char *malformed = "version=1\nboot_id=boot-a\nrequest_id=cp-1\nphase=complete\nphase=applying\nmode=1280x720@60\nresult=0\n";
    assert(carplay_display_parse_preparation_result(malformed,"boot-a","cp-1",1280,720,60)==0);
    assert(carplay_display_parse_preparation_result(NULL,"boot-a","cp-1",1280,720,60)==0);
    puts("display preparation status: PASS");
    return 0;
}
