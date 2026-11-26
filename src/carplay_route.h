#ifndef CARPLAY_ROUTE_H
#define CARPLAY_ROUTE_H
#include <stddef.h>
#include <stdint.h>
#define CARPLAY_ROUTE_COMPONENTS 8u
#define CARPLAY_ROUTE_WIRE_MAX 1200u
struct carplay_route_component { uint16_t id, current_chars, next_chars, destination_chars; };
struct carplay_route_components { unsigned count;struct carplay_route_component item[CARPLAY_ROUTE_COMPONENTS]; };
struct carplay_route_subscription { unsigned count,flags;uint16_t ids[CARPLAY_ROUTE_COMPONENTS]; };
struct carplay_route_snapshot {
    uint64_t sampled;
    uint32_t remaining,seconds,next_distance;
    uint16_t index;
    uint8_t state,maneuver;
    char current[256],next[256],destination[256],source[256];
};
int carplay_route_identify(const uint8_t *,size_t,struct carplay_route_components *);
int carplay_route_subscribe(const struct carplay_route_components *,const uint8_t *,size_t,struct carplay_route_subscription *);
int carplay_route_decode(const uint8_t *,size_t,uint64_t,struct carplay_route_snapshot *);
int carplay_route_read(uint64_t,struct carplay_route_snapshot *);
int carplay_route_update(const struct carplay_route_subscription *,const struct carplay_route_component *,
    const struct carplay_route_snapshot *,uint8_t *,size_t,size_t *);
int carplay_route_maneuver(const struct carplay_route_component *,const struct carplay_route_snapshot *,uint8_t *,size_t,size_t *);
#endif
