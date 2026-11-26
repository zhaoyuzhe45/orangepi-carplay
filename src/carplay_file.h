#ifndef CARPLAY_FILE_H
#define CARPLAY_FILE_H
#include <stddef.h>
#include <stdint.h>
#define CARPLAY_ART_MAX 32768u
/* phase: idle, setup, await-start, sending, paused, await-result. */
struct carplay_file { unsigned phase;uint8_t id;uint16_t type;size_t size,offset;uint64_t deadline;uint8_t data[CARPLAY_ART_MAX]; };
int carplay_file_begin(struct carplay_file *,uint8_t,const uint8_t *,size_t,uint16_t,uint64_t);
int carplay_file_next(struct carplay_file *,uint8_t *,size_t,size_t *,uint64_t);
void carplay_file_commit(struct carplay_file *,size_t,uint64_t);
void carplay_file_control(struct carplay_file *,const uint8_t *,size_t,uint64_t);
#endif
