#ifndef CARPLAY_MODES_H
#define CARPLAY_MODES_H

#include <stddef.h>
#include <stdint.h>

enum carplay_modes_entity {
    CARPLAY_ENTITY_NONE = 0,
    CARPLAY_ENTITY_CONTROLLER = 1,
    CARPLAY_ENTITY_ACCESSORY = 2
};

enum carplay_modes_resource {
    CARPLAY_RESOURCE_MAIN_SCREEN = 1,
    CARPLAY_RESOURCE_MAIN_AUDIO = 2
};

/* Caller serializes access to this session-local state. */
struct carplay_modes {
    int screen_entity;
    int screen_permanent_entity;
    int audio_entity;
    int audio_permanent_entity;
    int screen_borrowed;
    int audio_borrowed;
    int phone_call; /* Controller holds the telephony app state when true. */
    int audio_controller_enabled;
    /* MainScreen/MainAudio: take, borrow, unborrow constraints (100/500/1000). */
    int constraints[2][3];
};

/* Only claim MainAudio when that session actually supports/enables audio. */
void carplay_modes_init(struct carplay_modes *state, int audio_controller);

/* Full changeModes command bplist. 0 accepts all changes, -1 changes nothing.
 * Supports accessory Take(1)/Untake(2)/Borrow(3)/Unborrow(4).
 * App state transfers are refused. Repeated Borrow remains idempotent.
 * Untake leaves None; reclaim is a separate local transaction after response.
 */
int carplay_modes_apply(struct carplay_modes *state,
                        const uint8_t *body, size_t body_len);
/* Same transaction, with a static diagnostic string on rejection, NULL on
 * success. Does not retain the caller's body or change acceptance rules. */
int carplay_modes_apply_with_reason(struct carplay_modes *state,
    const uint8_t *body, size_t body_len, const char **reason);
int carplay_modes_screen_active(const struct carplay_modes *state);
int carplay_modes_audio_active(const struct carplay_modes *state);
/* Returns changed resource bits (1 screen, 2 audio), 0 no change, -1 invalid.
 * Only unowned resources are acquired; never overrides accessory constraints. */
int carplay_modes_reclaim_unowned(struct carplay_modes *state);

#endif
