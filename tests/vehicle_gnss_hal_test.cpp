#include "../system-patch/vehicle-gnss/VehicleGnss.cpp"
#include <assert.h>
class TestCallback final : public IGnssCallback {
public:
    int statusCount=0,capabilities=0;
    Return<void> gnssLocationCb(const GnssLocation&) override { return Void(); }
    Return<void> gnssStatusCb(GnssStatusValue) override { statusCount++;return Void(); }
    Return<void> gnssSvStatusCb(const GnssSvStatus&) override { return Void(); }
    Return<void> gnssNmeaCb(int64_t,const android::hardware::hidl_string&) override { return Void(); }
    Return<void> gnssSetCapabilitesCb(uint32_t caps) override { capabilities=caps;return Void(); }
    Return<void> gnssAcquireWakelockCb() override { return Void(); }
    Return<void> gnssReleaseWakelockCb() override { return Void(); }
    Return<void> gnssRequestTimeCb() override { return Void(); }
    Return<void> gnssSetSystemInfoCb(const GnssSystemInfo&) override { return Void(); }
};
int main() {
    assert(HIDL_FETCH_IGnss("other")==nullptr);
    sp<IGnss> gnss=HIDL_FETCH_IGnss("default");assert(gnss!=nullptr);
    sp<TestCallback> cb=new TestCallback();
    bool ok=gnss->setCallback(cb);assert(ok && cb->capabilities==1);
    ok=gnss->setPositionMode(IGnss::GnssPositionMode::STANDALONE,
            IGnss::GnssPositionRecurrence::RECURRENCE_PERIODIC,1000,0,0);assert(ok);
    ok=gnss->start();assert(ok && cb->statusCount==2);
    ok=gnss->stop();assert(ok && cb->statusCount==4);
    auto r=gnss->cleanup();assert(r.isOk());
    gnss=nullptr;
    puts("PASS Android 8.1 GNSS HIDL ABI, callbacks, start/stop and cleanup");
}
