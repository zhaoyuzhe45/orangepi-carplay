#include "../src/carplay_display.c"
#include <assert.h>
#ifdef __ANDROID__
#define TEST_SHELL "/system/bin/sh"
#else
#define TEST_SHELL "/bin/sh"
#endif
void iap2_log(const char *format, ...) { (void)format; }
static void check_job(const char *command, unsigned char expected)
{
    const char *args[]={TEST_SHELL,"-c",command,NULL};
    int fd=-1;assert(!launch_display_helper(args,&fd));assert(fd>=0);
    struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,10000)>0);
    unsigned char result=0;assert(read(fd,&result,1)==1 && result==expected);
    close(fd);
}
int main(void)
{
    check_job("exit 0",1);check_job("exit 70",2);
    const char *args[]={"/not/an/executable",NULL};int fd=-1;
    assert(!launch_display_helper(args,&fd));struct pollfd p={fd,POLLIN,0};
    assert(poll(&p,1,10000)>0);unsigned char result=0;
    assert(read(fd,&result,1)==1 && result==2);close(fd);
    /* An active helper must not retain the sender's sockets in its supervisor. */
    int sentinel[2];assert(pipe(sentinel)==0);
    const char *slow[]={TEST_SHELL,"-c","sleep 3; exit 0",NULL};
    assert(!launch_display_helper(slow,&fd));close(sentinel[1]);
    struct pollfd s={sentinel[0],POLLIN,0};assert(poll(&s,1,2000)>0);
    assert(s.revents&POLLHUP);close(sentinel[0]);
    close(fd); /* Teardown does not wait for the detached worker. */
    puts("display helper: successful exit/failure/exec failure/descriptor isolation PASS");
    return 0;
}
