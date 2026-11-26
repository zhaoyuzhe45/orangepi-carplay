#include "carplay_knob.h"
#include <limits.h>
#include <string.h>

enum { CENTER=1, HOME, BACK, X, Y, WHEEL };

static uint32_t unsigned_item(const uint8_t *p, unsigned n)
{
    uint32_t v=0;
    for (unsigned i=0;i<n;i++) v|=(uint32_t)p[i]<<(i*8u);
    return v;
}

static int32_t signed_item(uint32_t v, unsigned bits)
{
    if (bits && bits<32 && (v&(UINT32_C(1)<<(bits-1)))) v|=UINT32_MAX<<bits;
    return (int32_t)v;
}

static unsigned usage_action(uint32_t u)
{
    switch (u) {
    case 0x00090001: return CENTER;
    case 0x000c0223: return HOME;
    case 0x000c0224: return BACK;
    case 0x00010030: return X;
    case 0x00010031: return Y;
    case 0x00010038: return WHEEL;
    default: return 0;
    }
}

int carplay_knob_parse(const uint8_t *data, size_t len, struct carplay_knob_layout *out)
{
    struct globals { uint32_t page,size,count,id; int32_t min,max; } g={0},stack[4];
    struct carplay_knob_layout p={0};
    uint32_t usages[64],range_min=0,range_max=0;
    unsigned usage_count=0,have_min=0,have_max=0,depth=0,collections=0;
    size_t offset=0;
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    if (!data || !len || len>4096) return -1;
    while (offset<len) {
        uint8_t prefix=data[offset++];
        unsigned n=prefix&3u,type=(prefix>>2)&3u,tag=prefix>>4;
        if (prefix==0xfe) return -1;
        if (n==3) n=4;
        if (n>len-offset) return -1;
        uint32_t value=unsigned_item(data+offset,n);
        offset+=n;
        if (type==3) return -1;
        if (type==1) {
            switch (tag) {
            case 0: if (value>65535) return -1; g.page=value; break;
            case 1: g.min=signed_item(value,n*8u); break;
            case 2:
                if (g.min>=0 && value>INT32_MAX) return -1;
                g.max=g.min<0?signed_item(value,n*8u):(int32_t)value;
                break;
            case 7: g.size=value; break;
            case 8: if (!value || value>255) return -1; g.id=value; break;
            case 9: g.count=value; break;
            case 10: if (n || depth==4) return -1; stack[depth++]=g; break;
            case 11: if (n || !depth) return -1; g=stack[--depth]; break;
            default: break;
            }
        } else if (type==2) {
            uint32_t u=n==4?value:(g.page<<16)|value;
            if (tag==0) { if (usage_count==64) return -1; usages[usage_count++]=u; }
            else if (tag==1) { range_min=u; have_min=1; }
            else if (tag==2) { range_max=u; have_max=1; }
            else if (tag==10) return -1; /* Alternative usage sets unsupported. */
        } else if (type==0) {
            if (tag==10) { if (n!=1 || ++collections>32) return -1; }
            else if (tag==12) { if (n || !collections) return -1; collections--; }
            else if (tag==8) {
                if (!n) return -1;
                if (!g.size || g.size>32 || !g.count || g.count>512/g.size) return -1;
                unsigned r=0;
                while (r<p.report_count && p.reports[r].id!=g.id) r++;
                if (r==p.report_count) {
                    if (r==CARPLAY_KNOB_REPORTS) return -1;
                    p.reports[r].id=(uint8_t)g.id;
                    p.report_count++;
                }
                unsigned start=p.reports[r].bits,bits=g.size*g.count;
                if (start+bits>512) return -1;
                p.reports[r].bits=(uint16_t)(start+bits);
                if (have_min!=have_max || (have_min &&
                    (range_max<range_min || range_max-range_min>511))) return -1;
                if (!(value&1u)) for (unsigned i=0;i<g.count;i++) {
                    uint32_t u=i<usage_count?usages[i]:have_min?
                        range_min+(i<range_max-range_min?i:range_max-range_min):
                        usage_count?usages[usage_count-1]:0;
                    unsigned action=usage_action(u);
                    if (!action) continue;
                    if (!(value&2u) || g.min>g.max || p.field_count==CARPLAY_KNOB_FIELDS) return -1;
                    if (action<=BACK && (g.min!=0 || g.max!=1 || (value&4u))) return -1;
                    if (action>=X && (g.min>0 || g.max<0 || g.min==g.max)) return -1;
                    if (g.size<32) {
                        int64_t lo=g.min<0?-(INT64_C(1)<<(g.size-1)):0;
                        int64_t hi=(INT64_C(1)<<(g.min<0?g.size-1:g.size))-1;
                        if (g.min<lo || g.max>hi) return -1;
                    }
                    unsigned f=p.field_count++;
                    p.fields[f].offset=(uint16_t)(start+i*g.size);
                    p.fields[f].size=(uint8_t)g.size;
                    p.fields[f].report=(uint8_t)r;
                    p.fields[f].action=(uint8_t)action;
                    p.fields[f].relative=(uint8_t)((value&4u)!=0);
                    p.fields[f].minimum=g.min;
                    p.fields[f].maximum=g.max;
                }
            }
            usage_count=have_min=have_max=0;
        }
    }
    if (depth || collections || !p.field_count) return -1;
    for (unsigned r=0;r<p.report_count;r++)
        if (!p.reports[r].id && p.report_count>1) return -1;
    p.valid=1;
    *out=p;
    return 0;
}

int carplay_knob_owns_report(const struct carplay_knob_layout *p, const uint8_t *r, size_t n)
{
    if (!p || !p->valid || !r || !n) return 0;
    for (unsigned f=0;f<p->field_count;f++) {
        unsigned id=p->reports[p->fields[f].report].id;
        if (!id || r[0]==id) return 1;
    }
    return 0;
}

static int direction(int32_t v) { return (v>0)-(v<0); }

int carplay_knob_decode(const struct carplay_knob_layout *p, struct carplay_knob_state *state,
    const uint8_t *report, size_t len, struct carplay_key_batch *out)
{
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    if (!p || !p->valid || !state || !report || !len) return -1;
    unsigned r=0;
    if (p->reports[0].id) {
        while (r<p->report_count && p->reports[r].id!=report[0]) r++;
        if (r==p->report_count) return -1;
        report++; len--;
    }
    if (len!=(p->reports[r].bits+7u)/8u) return -1;
    struct carplay_knob_state next=*state;
    struct carplay_key_batch batch={0};
    for (unsigned f=0;f<p->field_count;f++) {
        if (p->fields[f].report!=r) continue;
        uint32_t raw=0;
        for (unsigned b=0;b<p->fields[f].size;b++) {
            unsigned bit=p->fields[f].offset+b;
            raw|=(uint32_t)((report[bit/8]>>(bit%8))&1u)<<b;
        }
        int32_t value=p->fields[f].minimum<0?signed_item(raw,p->fields[f].size):(int32_t)raw;
        if (value<p->fields[f].minimum || value>p->fields[f].maximum) return -1;
        unsigned action=p->fields[f].action;
        int key=0;
        uint32_t count=0;
        if (action<=BACK) {
            if (value && !state->values[f]) { key=action==CENTER?66:action==HOME?3:4; count=1; }
        } else if (value && (p->fields[f].relative || direction(value)!=direction(state->values[f]))) {
            key=(action==X || action==WHEEL)?(value>0?22:21):(value>0?20:19);
            count=p->fields[f].relative?(uint32_t)(value<0?-(int64_t)value:value):1u;
        }
        next.values[f]=value;
        if (count) {
            unsigned i=batch.count++;
            batch.entries[i].keycode=key;
            batch.entries[i].count=count;
        }
    }
    *state=next;
    *out=batch;
    return 0;
}
