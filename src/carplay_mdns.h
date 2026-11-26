#ifndef CARPLAY_MDNS_H
#define CARPLAY_MDNS_H

#include <stdint.h>

struct carplay_mdns;

/* Browse only ifname. found runs on the discovery worker; address is a numeric
 * IPv4 or IPv6 address (link-local IPv6 includes %ifname), valid during this
 * callback only. The callback must return promptly and must not call stop.
 * Returns 0 on success, -1 with errno on failure; *out is NULL on failure. */
int carplay_mdns_start(struct carplay_mdns **out, const char *ifname,
                      void (*found)(void *, const char *, uint16_t), void *ctx);

/* Call from the owner, never from found. Wakes and joins the worker before
 * releasing sockets and callback context; no callback can run after return.
 * Stop calls for one handle must not overlap. NULL is safe. */
void carplay_mdns_stop(struct carplay_mdns *mdns);

#endif
