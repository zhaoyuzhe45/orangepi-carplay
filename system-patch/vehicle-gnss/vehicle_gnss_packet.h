#ifndef VEHICLE_GNSS_PACKET_H
#define VEHICLE_GNSS_PACKET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
typedef struct {
    uint32_t flags;
    uint64_t revision, utc_ms, elapsed_ns;
    double latitude, longitude, altitude;
    float speed, bearing, accuracy;
} VehicleGnssPacket;
static inline uint64_t vehicle_gnss_u64(const unsigned char *p) {
    uint64_t n=0; for (int i=0;i<8;i++) n=(n<<8)|p[i]; return n;
}
static inline uint32_t vehicle_gnss_u32(const unsigned char *p) {
    uint32_t n=0; for (int i=0;i<4;i++) n=(n<<8)|p[i]; return n;
}
static inline double vehicle_gnss_double(const unsigned char *p) {
    uint64_t n=vehicle_gnss_u64(p); double d; memcpy(&d,&n,8); return d;
}
static inline float vehicle_gnss_float(const unsigned char *p) {
    uint32_t n=vehicle_gnss_u32(p); float f; memcpy(&f,&n,4); return f;
}
static inline bool vehicle_gnss_decode(const unsigned char *data, size_t size,
        VehicleGnssPacket *out) {
    if (!data || !out || size!=80 || memcmp(data,"CVG1",4)) return false;
    VehicleGnssPacket p;
    p.flags=vehicle_gnss_u32(data+4); p.revision=vehicle_gnss_u64(data+8);
    if (!p.revision || (p.flags & ~31U)) return false;
    p.utc_ms=vehicle_gnss_u64(data+16); p.elapsed_ns=vehicle_gnss_u64(data+24);
    p.latitude=vehicle_gnss_double(data+32); p.longitude=vehicle_gnss_double(data+40);
    p.altitude=vehicle_gnss_double(data+48); p.speed=vehicle_gnss_float(data+56);
    p.bearing=vehicle_gnss_float(data+60); p.accuracy=vehicle_gnss_float(data+64);
    if (p.flags) {
        if ((p.flags & 17U)!=17U || !p.utc_ms ||
                !isfinite(p.latitude) || fabs(p.latitude)>90 ||
                !isfinite(p.longitude) || fabs(p.longitude)>180 ||
                !isfinite(p.accuracy) || p.accuracy<=0 || p.accuracy>100000) return false;
        if ((p.flags&2U) && (!isfinite(p.altitude) || p.altitude < -21000 || p.altitude>101000)) return false;
        if ((p.flags&4U) && (!isfinite(p.speed) || p.speed<0 || p.speed>1030)) return false;
        if ((p.flags&8U) && (!isfinite(p.bearing) || p.bearing<0 || p.bearing>=360)) return false;
    }
    *out=p; return true;
}
#endif
