#ifndef CARPLAY_CALLS_H
#define CARPLAY_CALLS_H
#include <stddef.h>
#include <stdint.h>
#define CARPLAY_CALLS_MAX 8u
#define CARPLAY_CALL_WIRE_MAX 128u
struct carplay_call { char uuid[37],number[65];uint8_t status,direction; };
struct carplay_call_list { uint64_t sampled_ms;size_t count;struct carplay_call calls[CARPLAY_CALLS_MAX]; };
int carplay_calls_decode(const uint8_t *wire,size_t len,uint64_t now,struct carplay_call_list *out);
int carplay_calls_subscribe(uint32_t *fields,const uint8_t *body,size_t len);
int carplay_call_encode(uint32_t fields,const struct carplay_call *call,uint8_t *out,size_t cap,size_t *len);
int carplay_calls_read(uint64_t now,struct carplay_call_list *out);
#endif
