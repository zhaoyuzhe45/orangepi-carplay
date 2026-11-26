#define _GNU_SOURCE
#include "carplay_audio_bridge.h"
#include "airplay_bplist.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

extern int carplay_audio_bridge_legacy_nav_setup(struct carplay_audio_bridge *,
    struct airplay_stream_setup *) __attribute__((weak));
void iap2_log(const char *fmt, ...) { (void)fmt; }

static int receiver(uint16_t *port) {
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t size=sizeof(a);
    assert(fd>=0 && bind(fd,(void *)&a,sizeof(a))==0);
    assert(getsockname(fd,(void *)&a,&size)==0);
    *port=ntohs(a.sin_port);
    return fd;
}

static int connect_capture(const char *name) {
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un a={.sun_family=AF_UNIX};
    strcpy(a.sun_path+1,name);
    assert(fd>=0 && connect(fd,(void *)&a,
        offsetof(struct sockaddr_un,sun_path)+1+strlen(a.sun_path+1))==0);
    return fd;
}

static void send_frame(int ipc, int track_count) {
    uint8_t frame[3*3840];
    for(size_t i=0;i<sizeof(frame);i+=2){
        frame[i]=0;
        frame[i+1]=(uint8_t)(11+i/3840);
    }
    size_t offset=0, size=(size_t)track_count*3840;
    while(offset<size){
        ssize_t sent=write(ipc,frame+offset,size-offset);
        assert(sent>0);offset+=(size_t)sent;
    }
}

static uint32_t expect_packet(int fd,size_t bytes,uint8_t sample,const char *label) {
    uint8_t packet[1500];
    struct pollfd p={fd,POLLIN,0};
    for(int i=0;i<10;i++){
        assert(poll(&p,1,1000)==1);
        ssize_t n=recv(fd,packet,sizeof(packet),0);
        assert(n==(ssize_t)bytes && packet[0]==0x80 && packet[1]==(bytes==716?101:100));
        if(packet[12]==0)continue;
        for(size_t j=12;j<bytes;j+=2){
            if(packet[j]!=sample || packet[j+1]!=0){fprintf(stderr,"unexpected %s sample at %zu\n",label,j);assert(0);}
        }
        uint32_t ts;memcpy(&ts,packet+4,4);return ntohl(ts);
    }
    assert(!"no non-silent RTP packet");return 0;
}

static void drain(int fd) {
    uint8_t packet[1500];
    while(recv(fd,packet,sizeof(packet),MSG_DONTWAIT)>0){}
}

int main(void) {
    assert(carplay_audio_bridge_legacy_nav_setup && "legacy Alternate Audio support missing");
    struct carplay_audio_bridge *bridge=NULL;
    struct carplay_session_capabilities caps={0};
    caps.audio.format_count=2;
    caps.audio.formats[0].present=1;caps.audio.formats[0].type=100;
    caps.audio.formats[0].output_formats=0x8000;
    caps.audio.formats[1].present=1;caps.audio.formats[1].type=101;
    caps.audio.formats[1].output_formats=0x10;
    uint16_t media_port,nav_port;
    int media=receiver(&media_port),nav=receiver(&nav_port);
    assert(carplay_audio_bridge_init_legacy(&bridge,1,0)==0);
    assert(carplay_audio_bridge_prepare(bridge,&caps)==0);
    assert(carplay_audio_bridge_start_stream(bridge,100,"media",media_port,0,"127.0.0.1")==0);
    struct airplay_stream_setup setup={0};
    assert(carplay_audio_bridge_legacy_nav_setup(bridge,&setup)==0);
    int old=connect_capture(CARPLAY_AUDIO_CAPTURE_SOCKET);
    send_frame(old,1);
    expect_packet(media,1420,11,"v1 capture with new daemon");
    assert(carplay_audio_bridge_legacy_nav_setup(bridge,&setup)==0);
    drain(media);
    close(old);
    int ipc=connect_capture("carplay_audio_capture_test_v2");
    send_frame(ipc,3);send_frame(ipc,3);
    expect_packet(media,1420,11,"fallback mix");
    assert(carplay_audio_bridge_legacy_nav_setup(bridge,&setup)==1);
    assert(setup.type==101 && setup.audio_format==0x10 && !setup.input && setup.control_port &&
        !strcmp(setup.audio_type,"default"));
    usleep(20000);drain(media);
    assert(carplay_audio_bridge_start_stream(bridge,101,"default",nav_port,0,"127.0.0.1")==0);
    for(int i=0;i<4;i++)send_frame(ipc,3);
    expect_packet(media,1420,12,"clean music");
    uint32_t ts=expect_packet(nav,716,13,"isolated navigation");
    assert(expect_packet(nav,716,13,"navigation RTP continuity")-ts==352);
    usleep(20000);drain(media);drain(nav);
    carplay_audio_bridge_set_paused(bridge,1);
    send_frame(ipc,3);send_frame(ipc,3);
    struct pollfd inactive[]={{media,POLLIN,0},{nav,POLLIN,0}};
    assert(poll(inactive,2,30)==0);
    carplay_audio_bridge_set_paused(bridge,0);
    assert(carplay_audio_bridge_test_force_send_failure(bridge,101,"default")==0);
    send_frame(ipc,3);send_frame(ipc,3);
    for(int i=0;i<5;i++)expect_packet(media,1420,12,"music survives nav failure");
    send_frame(ipc,3);
    expect_packet(media,1420,11,"mixed fallback after 101 send failure");
    carplay_audio_bridge_destroy(&bridge);close(ipc);close(media);close(nav);
    puts("legacy nav101: v1 compatibility, v2 fallback, RTP isolation, pause and UDP failure passed");
    return 0;
}
