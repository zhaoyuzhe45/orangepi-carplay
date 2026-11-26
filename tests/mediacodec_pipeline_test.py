#!/usr/bin/env python3
"""Compile the production access-unit consumer with the real H.264 parser."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/airplay_client.c').read_text()
signature = 'static int consume_encoder_packet('
assert signature in source, 'live pipeline does not consume MediaCodec access units'
start = source.index(signature)
consumer = source[start:source.index('\n}', start) + 2]
harness = r'''
#include "airplay_h264.h"
#include "carplay_encoder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
struct airplay_client {int frames, slices, configs;};
static int flush_video_batch(struct airplay_client *c) {
    if(c->slices) { c->frames++; c->slices=0; } return 0;
}
static int config(void *p,const uint8_t *b,size_t n) {
    (void)b;(void)n; ((struct airplay_client*)p)->configs++; return 0;
}
static int frame(void *p,const uint8_t *b,size_t n,int key) {
    (void)b;(void)n;(void)key; ((struct airplay_client*)p)->slices++; return 0;
}
'''
cases = r'''
int main(void) {
    uint8_t buf[4096]; struct airplay_h264_parser parser;
    struct airplay_client c={0};
    const uint8_t csd[]={0,0,0,1,0x67,0x42,0,0x1f,0x80,0,0,1,0x68,0x80};
    const uint8_t au[]={0,0,0,1,0x65,0x80,0,0,1,0x65,0x40};
    struct carplay_encoder_packet p={csd,sizeof(csd),CARPLAY_ENCODER_CONFIG,10};
    airplay_h264_parser_init(&parser,buf,sizeof(buf),config,frame,&c);
    CHECK(consume_encoder_packet(&c,&parser,&p)==0 && c.configs==1 && c.frames==0);
    p.data=au; p.size=sizeof(au); p.flags=CARPLAY_ENCODER_KEYFRAME;
    CHECK(consume_encoder_packet(&c,&parser,&p)==0);
    CHECK(c.frames==1 && c.slices==0 && parser.nal_len==0);
    CHECK(consume_encoder_packet(&c,&parser,&p)==0 && c.frames==2);
    p.flags=CARPLAY_ENCODER_EOS; p.size=0;
    CHECK(consume_encoder_packet(&c,&parser,&p)<0 && c.frames==2);
    puts("PASS: config before frames, complete multi-slice access units flush immediately, EOS stops");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp) / 'pipeline.c'
    path.write_text(harness + consumer + cases)
    binary = Path(temp) / 'pipeline'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'src'), str(path), str(root/'src/airplay_h264.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
