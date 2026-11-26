#include "carplay_calls.h"
#include <stdio.h>
int main(void) {
    struct carplay_call_list calls;
    if(carplay_calls_read(0,&calls)<0) { puts("Call provider unavailable");return 1; }
    printf("Real Bluetooth call snapshot: count=%zu sampled_ms=%llu\n",calls.count,(unsigned long long)calls.sampled_ms);
    return 0;
}
