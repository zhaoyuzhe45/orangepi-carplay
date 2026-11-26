#include "../system-patch/vehicle-gnss/vehicle_gnss_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put64(unsigned char *p, uint64_t n) {
    for (int i=7;i>=0;i--) { p[i]=(unsigned char)n; n>>=8; }
}
static void put32(unsigned char *p, uint32_t n) {
    for (int i=3;i>=0;i--) { p[i]=(unsigned char)n; n>>=8; }
}
static void putDouble(unsigned char *p, double d) { uint64_t n; memcpy(&n,&d,8); put64(p,n); }
static void putFloat(unsigned char *p, float f) { uint32_t n; memcpy(&n,&f,4); put32(p,n); }
int main(void) {
    unsigned char b[80]={0}; VehicleGnssPacket p;
    memcpy(b,"CVG1",4);put32(b+4,31);put64(b+8,1);
    put64(b+16,1789992000000ULL);put64(b+24,10000000000ULL);
    putDouble(b+32,48.8567);putDouble(b+40,2.3517);putDouble(b+48,79.8);
    putFloat(b+56,10.910338F);putFloat(b+60,218.18F);putFloat(b+64,25);
    assert(vehicle_gnss_decode(b,80,&p));
    assert(p.flags==31 && p.revision==1 &&
            fabs(p.latitude-48.8567)<1e-12 && fabs(p.longitude-2.3517)<1e-12);
    assert(p.speed>10.9 && p.speed<11 && p.accuracy==25 && p.utc_ms==1789992000000ULL);
    assert(!vehicle_gnss_decode(b,79,&p));
    put64(b+24,1);assert(vehicle_gnss_decode(b,80,&p) && p.elapsed_ns==1);
    put64(b+24,UINT64_MAX);assert(vehicle_gnss_decode(b,80,&p) && p.elapsed_ns==UINT64_MAX);
    b[0]='X';assert(!vehicle_gnss_decode(b,80,&p));b[0]='C';
    putDouble(b+32,91);assert(!vehicle_gnss_decode(b,80,&p));putDouble(b+32,48.8567);
    putDouble(b+40,181);assert(!vehicle_gnss_decode(b,80,&p));putDouble(b+40,2.3517);
    putFloat(b+56,-1);assert(!vehicle_gnss_decode(b,80,&p));putFloat(b+56,10.910338F);
    putFloat(b+60,360);assert(!vehicle_gnss_decode(b,80,&p));putFloat(b+60,218.18F);
    putFloat(b+64,0);assert(!vehicle_gnss_decode(b,80,&p));putFloat(b+64,25);
    put64(b+32,0x7ff8000000000000ULL);assert(!vehicle_gnss_decode(b,80,&p));putDouble(b+32,48.8567);
    put32(b+4,32);assert(!vehicle_gnss_decode(b,80,&p));
    put32(b+4,0);assert(vehicle_gnss_decode(b,80,&p) && p.flags==0);
    puts("PASS GNSS frame endian, fields, validity and coordinate guards without age filtering");
    return 0;
}
