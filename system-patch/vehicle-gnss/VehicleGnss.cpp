// Vehicle-sourced GNSS entry for this Android 8.1 board, which has no GPS HAL.
#include <type_traits>
#include <array>
// The 1.0 interface ABI is unchanged; these aliases were added by later hidl-gen.
namespace android { namespace hardware {
template<class T> using hidl_bitfield = typename std::underlying_type<T>::type;
namespace details { struct i_tag {}; template<class T> inline constexpr std::array<T,0> hidl_enum_values{}; }
}}
#include <android/hardware/gnss/1.0/IGnss.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/system_properties.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "vehicle_gnss_packet.h"

using namespace android::hardware::gnss::V1_0;
using android::hardware::Return;
using android::hardware::Void;
using android::sp;

static bool trustedPeer(int fd) {
    ucred peer{}; socklen_t size=sizeof(peer);
    if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)) return false;
    FILE *file=fopen("/data/system/packages.list","re");
    if (!file) return false;
    char line[2048],name[256]; unsigned uid; bool accepted=false;
    while (fgets(line,sizeof(line),file)) {
        if (sscanf(line,"%255s %u",name,&uid)==2 && !strcmp(name,"com.carplaybox.audio")) {
            accepted=uid==peer.uid; break;
        }
    }
    fclose(file); return accepted;
}

class VehicleGnss final : public IGnss {
    pthread_mutex_t mutex_=PTHREAD_MUTEX_INITIALIZER;
    sp<IGnssCallback> callback_;
    VehicleGnssPacket latest_{};
    uint64_t emitted_=0;
    bool started_=false,quit_=false,threadCreated_=false;
    int listener_=-1;
    pthread_t thread_{};

    void status(IGnssCallback::GnssStatusValue value) {
        pthread_mutex_lock(&mutex_); sp<IGnssCallback> cb=callback_; pthread_mutex_unlock(&mutex_);
        if (cb!=nullptr) { auto r=cb->gnssStatusCb(value); (void)r.isOk(); }
    }
    void emit() {
        pthread_mutex_lock(&mutex_);
        if (!started_ || callback_==nullptr || !latest_.flags || latest_.revision==emitted_) {
            pthread_mutex_unlock(&mutex_); return;
        }
        VehicleGnssPacket p=latest_; sp<IGnssCallback> cb=callback_;
        emitted_=p.revision;
        pthread_mutex_unlock(&mutex_);
        GnssLocation location{};
        location.gnssLocationFlags=uint16_t(p.flags);
        location.latitudeDegrees=p.latitude; location.longitudeDegrees=p.longitude;
        location.altitudeMeters=p.altitude; location.speedMetersPerSec=p.speed;
        location.bearingDegrees=p.bearing; location.horizontalAccuracyMeters=p.accuracy;
        location.timestamp=p.utc_ms;
        auto r=cb->gnssLocationCb(location);
        if (!r.isOk()) __android_log_print(ANDROID_LOG_WARN,"CarPlayGnss","location callback failed");
    }
    static void *entry(void *p) { static_cast<VehicleGnss*>(p)->run(); return nullptr; }
    void run() {
        int client=-1; uint64_t revision=0; unsigned char frame[80]; size_t used=0;
        while (true) {
            pthread_mutex_lock(&mutex_); bool quit=quit_; pthread_mutex_unlock(&mutex_);
            if (quit) break;
            pollfd fds[2]={{listener_,POLLIN,0},{client,POLLIN,0}};
            int result=poll(fds,2,100);
            if (result<0 && errno!=EINTR) break;
            if (fds[0].revents&POLLIN) {
                int next=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC);
                if (next>=0) {
                    if (!trustedPeer(next)) { close(next); }
                    else {
                        if (client>=0) close(client);
                        client=next; revision=0; used=0;
                        pthread_mutex_lock(&mutex_); latest_={}; emitted_=0; pthread_mutex_unlock(&mutex_);
                    }
                }
            }
            if (client>=0 && fds[1].revents) {
                ssize_t n=recv(client,frame+used,sizeof(frame)-used,MSG_DONTWAIT);
                if (n>0) {
                    used+=size_t(n);
                    if (used==sizeof(frame)) {
                        VehicleGnssPacket p; unsigned char ack=2;
                        if (vehicle_gnss_decode(frame,sizeof(frame),&p) && p.revision>revision) {
                            revision=p.revision;
                            pthread_mutex_lock(&mutex_); latest_=p; pthread_mutex_unlock(&mutex_);
                            ack=1;
                        }
                        if (send(client,&ack,1,MSG_NOSIGNAL)!=1) shutdown(client,SHUT_RDWR);
                        used=0;
                    }
                } else if (n==0 || (errno!=EAGAIN && errno!=EINTR)) {
                    close(client); client=-1; used=0;
                    pthread_mutex_lock(&mutex_); latest_={}; pthread_mutex_unlock(&mutex_);
                }
            }
            emit();
        }
        if (client>=0) close(client);
    }
public:
    VehicleGnss() {
        listener_=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        const char *name="carplay.vehicle.gnss.v1";
        memcpy(address.sun_path+1,name,strlen(name));
        if (listener_>=0 && bind(listener_,reinterpret_cast<sockaddr*>(&address),
                offsetof(sockaddr_un,sun_path)+1+strlen(name))==0 && listen(listener_,2)==0) {
            threadCreated_=pthread_create(&thread_,nullptr,entry,this)==0;
        }
        if (!threadCreated_ && listener_>=0) { close(listener_); listener_=-1; }
        __system_property_set("sys.carplay.gnss.bridge",threadCreated_?"1":"0");
        __android_log_print(ANDROID_LOG_INFO,"CarPlayGnss","vehicle bridge ready=%d",threadCreated_);
    }
    ~VehicleGnss() override {
        pthread_mutex_lock(&mutex_);quit_=true;pthread_mutex_unlock(&mutex_);
        if (listener_>=0) shutdown(listener_,SHUT_RDWR);
        if (threadCreated_) pthread_join(thread_,nullptr);
        if (listener_>=0) close(listener_);
    }
    Return<bool> setCallback(const sp<IGnssCallback>& cb) override {
        if (!threadCreated_ || cb==nullptr) return false;
        pthread_mutex_lock(&mutex_);callback_=cb;pthread_mutex_unlock(&mutex_);
        auto r=cb->gnssSetCapabilitesCb(uint32_t(IGnssCallback::Capabilities::SCHEDULING));
        return r.isOk();
    }
    Return<bool> start() override {
        pthread_mutex_lock(&mutex_);started_=true;emitted_=0;pthread_mutex_unlock(&mutex_);
        status(IGnssCallback::GnssStatusValue::ENGINE_ON);
        status(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
        return threadCreated_;
    }
    Return<bool> stop() override {
        pthread_mutex_lock(&mutex_);started_=false;pthread_mutex_unlock(&mutex_);
        status(IGnssCallback::GnssStatusValue::SESSION_END);
        status(IGnssCallback::GnssStatusValue::ENGINE_OFF);return true;
    }
    Return<void> cleanup() override {
        auto r=stop();(void)r.isOk();
        pthread_mutex_lock(&mutex_);callback_=nullptr;latest_={};pthread_mutex_unlock(&mutex_);return Void();
    }
    Return<bool> injectTime(int64_t,int64_t,int32_t) override { return true; }
    Return<bool> injectLocation(double,double,float) override { return true; }
    Return<void> deleteAidingData(GnssAidingData) override { return Void(); }
    Return<bool> setPositionMode(GnssPositionMode,GnssPositionRecurrence recurrence,
            uint32_t,uint32_t,uint32_t) override {
        // The external source supplies the cadence; do not hold its fixes.
        return recurrence==GnssPositionRecurrence::RECURRENCE_PERIODIC;
    }
#define EMPTY_EXTENSION(Type,Name) Return<sp<Type>> Name() override { return sp<Type>{}; }
    EMPTY_EXTENSION(IAGnssRil,getExtensionAGnssRil)
    EMPTY_EXTENSION(IGnssGeofencing,getExtensionGnssGeofencing)
    EMPTY_EXTENSION(IAGnss,getExtensionAGnss)
    EMPTY_EXTENSION(IGnssNi,getExtensionGnssNi)
    EMPTY_EXTENSION(IGnssMeasurement,getExtensionGnssMeasurement)
    EMPTY_EXTENSION(IGnssNavigationMessage,getExtensionGnssNavigationMessage)
    EMPTY_EXTENSION(IGnssXtra,getExtensionXtra)
    EMPTY_EXTENSION(IGnssConfiguration,getExtensionGnssConfiguration)
    EMPTY_EXTENSION(IGnssDebug,getExtensionGnssDebug)
    EMPTY_EXTENSION(IGnssBatching,getExtensionGnssBatching)
#undef EMPTY_EXTENSION
};

extern "C" __attribute__((visibility("default"))) IGnss *HIDL_FETCH_IGnss(const char *name) {
    return name && !strcmp(name,"default") ? new VehicleGnss() : nullptr;
}
