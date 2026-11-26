#define _GNU_SOURCE
#include "carplay_audio_bridge.h"
#include "airplay_bplist.h"
#include "fixtures/legacy_audio_info_fixtures.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Weak declarations allow the red test to report the absent feature, rather
 * than fail at the linker before any assertion runs. */
extern int carplay_audio_bridge_init_legacy(struct carplay_audio_bridge **, int, int) __attribute__((weak));
extern int carplay_audio_bridge_legacy_select(struct carplay_audio_bridge *, int) __attribute__((weak));
extern int carplay_audio_bridge_microphone_demand(struct carplay_audio_bridge *) __attribute__((weak));
void iap2_log(const char *fmt, ...) { (void)fmt; }

/* Hold an actual IPC write in flight to test the Borrow return boundary. */
static _Atomic int hold_ipc, ipc_entered, release_ipc, pause_entered, pause_returned;
ssize_t send(int fd, const void *buffer, size_t length, int flags) {
    const uint8_t *p=buffer;
    if(atomic_load(&hold_ipc) && length>=20 && !memcmp(p,"CPM1",4) && p[4]==2) {
        atomic_store(&ipc_entered,1);
        while(!atomic_load(&release_ipc)) usleep(1000);
    }
    return syscall(SYS_sendto,fd,buffer,length,flags,NULL,0);
}
static void await_flag(_Atomic int *flag) {
    for(int i=0;i<1500 && !atomic_load(flag);i++) usleep(1000);
    assert(atomic_load(flag));
}
static void *pause_bridge(void *opaque) {
    atomic_store(&pause_entered,1);
    carplay_audio_bridge_set_paused(opaque,1);
    atomic_store(&pause_returned,1);
    return NULL;
}

static int udp(void) {
    int fd=socket(AF_INET,SOCK_DGRAM,0); assert(fd>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(fd,(void *)&a,sizeof(a))==0); return fd;
}
static unsigned port(int fd) {
    struct sockaddr_in a; socklen_t n=sizeof(a);
    assert(getsockname(fd,(void *)&a,&n)==0); return ntohs(a.sin_port);
}
static int receive(int fd,uint8_t *p) {
    struct pollfd f={fd,POLLIN,0}; assert(poll(&f,1,1500)==1);
    return (int)recv(fd,p,4096,0);
}
static uint32_t le32(const uint8_t *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void command(int fd,unsigned type) {
    uint8_t p[20]={'C','P','M','1'};p[4]=(uint8_t)type;
    assert(send(fd,p,sizeof(p),0)==sizeof(p));
}

static void jaguar_audio_capabilities(const uint8_t *data,size_t len) {
    struct carplay_session_capabilities caps;
    assert(carplay_capabilities_parse(data,len,&caps)==0);
    assert(caps.display.present && caps.display.width_pixels==1280 && caps.display.height_pixels==720);
    assert(caps.audio.format_count==8 && caps.audio.media_output && caps.audio.microphone_input);
    struct carplay_audio_bridge *bridge=NULL;
    assert(carplay_audio_bridge_init_legacy(&bridge,1,1)==0);
    assert(carplay_audio_bridge_prepare(bridge,&caps)==0);
    struct carplay_audio_status status;carplay_audio_bridge_get_status(bridge,&status);
    assert(status.media_available && status.microphone_available && "Jaguar string /info must enable legacy audio");
    struct airplay_stream_setup setup={0};struct carplay_audio_pcm_format format;
    assert(carplay_audio_bridge_fill_setups(bridge,&setup,1)==0);
    assert(!strcmp(setup.audio_type,"media") && setup.audio_format==0x8000 && !setup.input);
    assert(carplay_audio_lpcm_format(setup.audio_format,&format)==0 && format.sample_rate==48000 && format.channels==2);
    assert(carplay_audio_bridge_legacy_select(bridge,1)==0);
    assert(carplay_audio_bridge_fill_setups(bridge,&setup,1)==0);
    assert(!strcmp(setup.audio_type,"telephony") && setup.audio_format==0x40 && setup.input);
    assert(carplay_audio_lpcm_format(setup.audio_format,&format)==0 && format.sample_rate==24000 && format.channels==1);
    carplay_audio_bridge_destroy(&bridge);
}
static void audio_numeric_capabilities(void) {
    jaguar_audio_capabilities(jaguar_string_audio_info,sizeof(jaguar_string_audio_info));
    jaguar_audio_capabilities(jaguar_decimal_audio_info,sizeof(jaguar_decimal_audio_info));
    jaguar_audio_capabilities(jaguar_integer_audio_info,sizeof(jaguar_integer_audio_info));
    struct carplay_session_capabilities caps;
    for(size_t i=0;i<sizeof(legacy_audio_number_fixtures)/sizeof(legacy_audio_number_fixtures[0]);i++) {
        const struct legacy_audio_number_fixture *fixture=&legacy_audio_number_fixtures[i];
        assert(carplay_capabilities_parse(fixture->data,fixture->len,&caps)==0 && caps.audio.format_count==1);
        struct carplay_audio_format_capabilities *entry=&caps.audio.formats[0];
        if(entry->type!=fixture->type || entry->output_formats!=fixture->output || entry->input_formats!=fixture->input)
            fprintf(stderr,"numeric capability failed: %s type=%d out=0x%x in=0x%x\n",fixture->label,entry->type,entry->output_formats,entry->input_formats);
        assert(entry->type==fixture->type && entry->output_formats==fixture->output && entry->input_formats==fixture->input);
    }
    assert(carplay_capabilities_parse(jaguar_string_geometry_info,sizeof(jaguar_string_geometry_info),&caps)==0);
    assert(!caps.display.present && "Audio number compatibility must not change display parsing");
    puts("Jaguar string/decimal/integer info selects media 48000 stereo and telephony 24000 mono; numeric boundaries passed");
}

static void audio_selection_capabilities(void) {
    const size_t count = sizeof(legacy_audio_selection_fixtures) / sizeof(legacy_audio_selection_fixtures[0]);
    for (size_t i = 0; i < count; i++) {
        const struct legacy_audio_selection_fixture *fixture = &legacy_audio_selection_fixtures[i];
        struct carplay_session_capabilities caps;
        struct carplay_audio_bridge *bridge = NULL;
        struct carplay_audio_status status;
        assert(carplay_capabilities_parse(fixture->data, fixture->len, &caps) == 0);
        assert(carplay_audio_bridge_init_legacy(&bridge, fixture->media_enabled, fixture->mic_enabled) == 0);
        assert(carplay_audio_bridge_prepare(bridge, &caps) == 0);
        carplay_audio_bridge_get_status(bridge, &status);
        if (status.media_available != !!fixture->media_format ||
            status.microphone_available != !!fixture->call_format)
            fprintf(stderr, "selection failed: %s media=%d microphone=%d expected=0x%x/0x%x\n",
                    fixture->label, status.media_available, status.microphone_available,
                    fixture->media_format, fixture->call_format);
        assert(status.media_available == !!fixture->media_format);
        assert(status.microphone_available == !!fixture->call_format);
        for (int call = 0; call < 2; call++) {
            uint32_t expected = call ? fixture->call_format : fixture->media_format;
            int selected = carplay_audio_bridge_legacy_select(bridge, call);
            assert(expected ? selected == 0 : selected < 0);
            if (!expected) continue;
            struct airplay_stream_setup setup = {0};
            assert(carplay_audio_bridge_fill_setups(bridge, &setup, 1) == 0);
            assert(setup.type == 100 && setup.audio_format == expected && setup.input == call);
            assert(!strcmp(setup.audio_type, call ? "telephony" : "media"));
        }
        carplay_audio_bridge_destroy(&bridge);
    }
    printf("legacy audio selection: %zu real plist/bridge cases passed\n", count);
}

struct rate_consumer {
    int fd;
    unsigned rate;
    _Atomic unsigned delivered;
    _Atomic int invalid;
};
static void *consume_rate_pcm(void *opaque) {
    struct rate_consumer *consumer=opaque;
    uint8_t header[20],pcm[704];
    while(recv(consumer->fd,header,sizeof(header),MSG_WAITALL)==sizeof(header)) {
        if(memcmp(header,"CPM1",4) || le32(header+4)!=2 || le32(header+8)!=sizeof(pcm) ||
           le32(header+12)!=consumer->rate || le32(header+16)!=1 ||
           recv(consumer->fd,pcm,sizeof(pcm),MSG_WAITALL)!=sizeof(pcm)) {
            atomic_store(&consumer->invalid,1);break;
        }
        unsigned expected=atomic_load(&consumer->delivered);
        if((unsigned)(pcm[0]|((unsigned)pcm[1]<<8))!=expected)
            atomic_store(&consumer->invalid,1);
        atomic_fetch_add(&consumer->delivered,1);
    }
    return NULL;
}
static void sustained_microphone_rate(unsigned rate,uint32_t mask) {
    struct carplay_audio_bridge *bridge=NULL;
    struct carplay_session_capabilities caps={0};caps.audio.format_count=1;
    struct carplay_audio_format_capabilities *format=&caps.audio.formats[0];
    format->present=1;format->type=100;strcpy(format->audio_type,"telephony");
    format->input_formats=format->output_formats=mask;
    assert(carplay_audio_bridge_init_legacy(&bridge,0,1)==0);
    assert(carplay_audio_bridge_prepare(bridge,&caps)==0);
    assert(carplay_audio_bridge_legacy_select(bridge,1)==0);
    struct airplay_stream_setup setup={0};
    assert(carplay_audio_bridge_fill_setups(bridge,&setup,1)==0);
    int output=udp(),control=udp();
    assert(carplay_audio_bridge_start_stream(bridge,100,"telephony",port(output),port(control),"127.0.0.1")==0);
    int ipc=socket(AF_UNIX,SOCK_STREAM,0);assert(ipc>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    strcpy(address.sun_path+1,"carplay_audio_microphone_test_v1");
    assert(connect(ipc,(void *)&address,offsetof(struct sockaddr_un,sun_path)+1+strlen(address.sun_path+1))==0);
    uint8_t header[20];
    assert(recv(ipc,header,sizeof(header),MSG_WAITALL)==sizeof(header) && le32(header+12)==rate);
    command(ipc,5);command(ipc,3);usleep(50000);
    struct rate_consumer consumer={.fd=ipc,.rate=rate};pthread_t reader;
    assert(pthread_create(&reader,NULL,consume_rate_pcm,&consumer)==0);
    uint8_t packet[12+704]={0x80,100};
    struct sockaddr_in destination={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(setup.data_port)};
    struct timespec base;clock_gettime(CLOCK_MONOTONIC,&base);
    const unsigned count=300;
    for(unsigned i=0;i<count;i++) {
        packet[2]=(uint8_t)(i>>8);packet[3]=(uint8_t)i;
        packet[12]=(uint8_t)(i>>8);packet[13]=(uint8_t)i;
        assert(sendto(output,packet,sizeof(packet),0,(void *)&destination,sizeof(destination))==sizeof(packet));
        /* Command processing must stay responsive while PCM is continuously queued. */
        if(i==100) command(ipc,4);
        if(i==120) assert(!carplay_audio_bridge_microphone_demand(bridge));
        if(i==150) command(ipc,3);
        if(i==170) assert(carplay_audio_bridge_microphone_demand(bridge));
        uint64_t target=(uint64_t)base.tv_sec*1000000000u+base.tv_nsec+(uint64_t)(i+1)*352u*1000000000u/rate;
        struct timespec deadline={(time_t)(target/1000000000u),(long)(target%1000000000u)};
        while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL)==EINTR) { }
    }
    for(unsigned i=0;i<400 && atomic_load(&consumer.delivered)<count;i++) usleep(1000);
    struct carplay_audio_status status;carplay_audio_bridge_get_status(bridge,&status);
    fprintf(stderr,"sustained microphone rate=%u sent=%u received=%llu delivered=%u invalid=%d\n",
            rate,count,(unsigned long long)status.microphone_packets_received,
            atomic_load(&consumer.delivered),atomic_load(&consumer.invalid));
    assert(status.microphone_packets_received==count);
    assert(atomic_load(&consumer.delivered)==count && !atomic_load(&consumer.invalid));
    shutdown(ipc,SHUT_RDWR);assert(pthread_join(reader,NULL)==0);
    close(ipc);close(output);close(control);carplay_audio_bridge_destroy(&bridge);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(carplay_audio_bridge_init_legacy && "legacy bridge is not implemented");
    audio_selection_capabilities();
    if (argc == 2 && !strcmp(argv[1], "--selection-only")) return 0;
    assert(argc == 1);
    audio_numeric_capabilities();
    struct carplay_audio_bridge *b=NULL;
    assert(carplay_audio_bridge_init_legacy(&b,1,1)==0);
    struct carplay_session_capabilities caps={0}; caps.audio.format_count=3;
    struct carplay_audio_format_capabilities *f=caps.audio.formats;
    f[0].present=1;f[0].type=100;strcpy(f[0].audio_type,"media");f[0].output_formats=0x8000;
    f[1].present=1;f[1].type=100;strcpy(f[1].audio_type,"telephony");f[1].output_formats=0x50;f[1].input_formats=0x10;
    f[2].present=1;f[2].type=100;strcpy(f[2].audio_type,"alert");f[2].output_formats=0x800;
    assert(carplay_audio_bridge_prepare(b,&caps)==0);
    assert(carplay_audio_bridge_setup_count(b)==1);
    struct airplay_stream_setup s={0}; assert(carplay_audio_bridge_fill_setups(b,&s,1)==0);
    assert(s.connection_id==0 && !strcmp(s.audio_type,"media") && s.audio_format==0x8000 && !s.input);
    char name[64];size_t count=0;
    assert(carplay_audio_bridge_resolve_response(b,100,0,name,sizeof(name),&count)==0 && !strcmp(name,"media"));
    assert(carplay_audio_bridge_resolve_response(b,101,0,name,sizeof(name),&count)<0);
    uint8_t encoded[2048];size_t len;
    assert(airplay_bplist_encode_stream_setup(encoded,sizeof(encoded),&len,&s,1)==0);
    struct airplay_bplist_document d;struct airplay_bplist_value root,arr,entry,value;
    assert(airplay_bplist_init(&d,encoded,len)==0 && airplay_bplist_root(&d,&root)==0);
    assert(airplay_bplist_dict_get(&d,&root,"streams",&arr)==1 && airplay_bplist_array_get(&d,&arr,0,&entry)==0);
    assert(airplay_bplist_dict_get(&d,&entry,"controlPort",&value)==1);
    assert(airplay_bplist_dict_get(&d,&entry,"audioLatencyMs",&value)==1);
    assert(airplay_bplist_dict_get(&d,&entry,"streamID",&value)==0);
    int out=udp(),ctrl=udp();assert(carplay_audio_bridge_start_stream(b,100,"media",port(out),port(ctrl),"127.0.0.1")==0);
    uint8_t pcm[3840],wire[4096];for(size_t i=0;i<sizeof(pcm);i+=2){pcm[i]=0x34;pcm[i+1]=0x12;}
    assert(carplay_audio_bridge_submit_capture_pcm(b,pcm,sizeof(pcm))==0);
    int n=receive(out,wire);assert(n==1420 && wire[0]==0x80 && wire[1]==100 && wire[12]==0x12 && wire[13]==0x34);
    /* No Poly1305/nonce and no nonce increment is needed on this path. */
    assert(carplay_audio_bridge_legacy_select(b,1)==0);
    memset(&s,0,sizeof(s));assert(carplay_audio_bridge_fill_setups(b,&s,1)==0);
    assert(s.input && s.data_port && s.audio_format==0x10 && !strcmp(s.audio_type,"telephony"));
    assert(carplay_audio_bridge_start_stream(b,100,"telephony",port(out),port(ctrl),"127.0.0.1")==0);
    while(recv(out,wire,sizeof(wire),MSG_DONTWAIT)>0) { }
    /* Car mic reaches IPC, never the outbound car RTP socket. */
    int ipc=socket(AF_UNIX,SOCK_STREAM,0);assert(ipc>=0);
    struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path+1,"carplay_audio_microphone_test_v1");
    assert(connect(ipc,(void *)&a,offsetof(struct sockaddr_un,sun_path)+1+strlen(a.sun_path+1))==0);
    assert(recv(ipc,wire,20,MSG_WAITALL)==20 && !memcmp(wire,"CPM1",4) && le32(wire+4)==1 && le32(wire+12)==16000);
    command(ipc,5);command(ipc,3);usleep(50000);
    assert(carplay_audio_bridge_microphone_demand(b)==1);
    uint8_t mic[16]={0x80,100,0,0,0,0,0,0,0,0,0,0,0x12,0x34,0x56,0x78};
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(s.data_port)};
    assert(sendto(out,mic,sizeof(mic),0,(void *)&dest,sizeof(dest))==sizeof(mic));
    assert(recv(ipc,wire,24,MSG_WAITALL)==24 && le32(wire+4)==2 && le32(wire+8)==4 && wire[20]==0x34 && wire[21]==0x12);
    struct pollfd output_ready={out,POLLIN,0};
    if(poll(&output_ready,1,50)>0) {
        int bytes=(int)recv(out,wire,sizeof(wire),0);
        /* A silence keepalive is permitted; captured microphone is not. */
        for(int i=12;i<bytes;i++) assert(wire[i]==0);
    }
    struct carplay_audio_status status;
    carplay_audio_bridge_get_status(b,&status);
    assert(status.media_active && status.microphone_active && status.microphone_ready && status.microphone_packets_received==1);
    atomic_store(&hold_ipc,1);
    assert(sendto(out,mic,sizeof(mic),0,(void *)&dest,sizeof(dest))==sizeof(mic));
    await_flag(&ipc_entered);
    pthread_t pauser;assert(pthread_create(&pauser,NULL,pause_bridge,b)==0);
    await_flag(&pause_entered);usleep(50000);
    assert(!atomic_load(&pause_returned) && "Borrow must wait for an in-flight microphone IPC write");
    atomic_store(&release_ipc,1);assert(pthread_join(pauser,NULL)==0);
    carplay_audio_bridge_get_status(b,&status);
    assert(!status.media_active && !status.microphone_active);
    carplay_audio_bridge_set_paused(b,0);
    command(ipc,4);usleep(50000);assert(!carplay_audio_bridge_microphone_demand(b));
    close(ipc);close(out);close(ctrl);carplay_audio_bridge_destroy(&b);assert(!b);
    atomic_store(&hold_ipc,0);
    sustained_microphone_rate(44100,0x400);
    sustained_microphone_rate(48000,0x4000);
    puts("legacy audio framing, format intersection, single stream and microphone IPC passed");
    return 0;
}
