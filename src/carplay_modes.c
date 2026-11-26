#include "carplay_modes.h"
#include "airplay_bplist.h"

#include <string.h>

#define MODES_MAX_BODY_BYTES (64u * 1024u)

static int owner_valid(int entity, int permanent, int borrowed)
{
    return (permanent == CARPLAY_ENTITY_NONE || permanent == CARPLAY_ENTITY_CONTROLLER ||
            permanent == CARPLAY_ENTITY_ACCESSORY) &&
           (borrowed == 0 || borrowed == 1) &&
           entity == (borrowed ? CARPLAY_ENTITY_ACCESSORY : permanent);
}

static int state_valid(const struct carplay_modes *state)
{
    if (!state)
        return 0;
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < 3; j++) {
            int constraint = state->constraints[i][j];
            if (constraint != 100 && constraint != 500 && constraint != 1000)
                return 0;
        }
    }
    return (state->audio_controller_enabled == 0 || state->audio_controller_enabled == 1) &&
           owner_valid(state->screen_entity, state->screen_permanent_entity,
                       state->screen_borrowed) &&
           owner_valid(state->audio_entity, state->audio_permanent_entity,
                       state->audio_borrowed);
}

void carplay_modes_init(struct carplay_modes *state, int audio_controller)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->screen_entity = CARPLAY_ENTITY_CONTROLLER;
    state->screen_permanent_entity = CARPLAY_ENTITY_CONTROLLER;
    state->audio_entity = audio_controller ? CARPLAY_ENTITY_CONTROLLER :
                                             CARPLAY_ENTITY_ACCESSORY;
    state->audio_permanent_entity = state->audio_entity;
    state->audio_controller_enabled = !!audio_controller;
    for (unsigned i = 0; i < 2; i++)
        for (unsigned j = 0; j < 3; j++)
            state->constraints[i][j] = 100;
}

int carplay_modes_screen_active(const struct carplay_modes *state)
{
    return state_valid(state) &&
           state->screen_entity == CARPLAY_ENTITY_CONTROLLER;
}

int carplay_modes_audio_active(const struct carplay_modes *state)
{
    return state_valid(state) &&
           state->audio_entity == CARPLAY_ENTITY_CONTROLLER;
}

static int required_number(const struct airplay_bplist_document *doc,
                            const struct airplay_bplist_value *dict,
                            const char *key, uint64_t *number)
{
    struct airplay_bplist_value value;
    return airplay_bplist_dict_get(doc, dict, key, &value) == 1 &&
           airplay_bplist_get_uint64(doc, &value, number) == 0 ? 0 : -1;
}

static int optional_number(const struct airplay_bplist_document *doc,
                            const struct airplay_bplist_value *dict,
                            const char *key, uint64_t *number)
{
    struct airplay_bplist_value value;
    int found = airplay_bplist_dict_get(doc, dict, key, &value);
    if (found <= 0)
        return found;
    return airplay_bplist_get_uint64(doc, &value, number) == 0 ? 1 : -1;
}

int carplay_modes_reclaim_unowned(struct carplay_modes *state)
{
    int changed = 0;
    if (!state_valid(state))
        return -1;
    if (state->screen_entity == CARPLAY_ENTITY_NONE &&
        state->screen_permanent_entity == CARPLAY_ENTITY_NONE && !state->screen_borrowed) {
        state->screen_entity = state->screen_permanent_entity = CARPLAY_ENTITY_CONTROLLER;
        changed |= 1;
    }
    if (state->audio_controller_enabled && state->audio_entity == CARPLAY_ENTITY_NONE &&
        state->audio_permanent_entity == CARPLAY_ENTITY_NONE && !state->audio_borrowed) {
        state->audio_entity = state->audio_permanent_entity = CARPLAY_ENTITY_CONTROLLER;
        changed |= 2;
    }
    return changed;
}

static int modes_rejected(const char **reason, const char *message)
{
    if (reason) *reason = message;
    return -1;
}

int carplay_modes_apply_with_reason(struct carplay_modes *state,
    const uint8_t *body, size_t body_len, const char **reason)
{
    struct airplay_bplist_document doc;
    struct airplay_bplist_value root, params, resources, value;
    struct carplay_modes next;
    uint64_t count;
    unsigned seen = 0;
    int found;

    if (reason) *reason = NULL;

    if (!state_valid(state) || !body || body_len > MODES_MAX_BODY_BYTES ||
        airplay_bplist_init(&doc, body, body_len) < 0 ||
        airplay_bplist_root(&doc, &root) < 0 ||
        root.type != AIRPLAY_BPLIST_DICTIONARY ||
        airplay_bplist_dict_get(&doc, &root, "type", &value) != 1 ||
        !airplay_bplist_string_equals(&doc, &value, "changeModes") ||
        airplay_bplist_dict_get(&doc, &root, "params", &params) != 1 ||
        params.type != AIRPLAY_BPLIST_DICTIONARY ||
        airplay_bplist_dict_get(&doc, &params, "resources", &resources) != 1 ||
        airplay_bplist_array_count(&doc, &resources, &count) < 0 ||
        count == 0 || count > 2)
        return modes_rejected(reason, "invalid state, request or resource list");

    /* Inbound app-state commands are not implemented. Phone observations
     * alone do not authorize remote app-state transitions.
     */
    found = airplay_bplist_dict_get(&doc, &params, "appStates", &value);
    if (found < 0 || (found == 1 &&
        (value.type != AIRPLAY_BPLIST_ARRAY || value.count != 0)))
        return modes_rejected(reason, "appStates malformed or transitions unsupported");

    next = *state;
    for (size_t i = 0; i < (size_t)count; i++) {
        struct airplay_bplist_value resource;
        uint64_t id, transfer;
        int *entity, *borrowed, *permanent;
        unsigned bit;

        if (airplay_bplist_array_get(&doc, &resources, i, &resource) < 0 ||
            resource.type != AIRPLAY_BPLIST_DICTIONARY ||
            required_number(&doc, &resource, "resourceID", &id) < 0 ||
            required_number(&doc, &resource, "transferType", &transfer) < 0 ||
            (id != CARPLAY_RESOURCE_MAIN_SCREEN &&
             id != CARPLAY_RESOURCE_MAIN_AUDIO) ||
            (transfer < 1 || transfer > 4))
            return modes_rejected(reason, "invalid resourceID or transferType");
        bit = 1u << (unsigned)id;
        if (seen & bit)
            return modes_rejected(reason, "duplicate resourceID");
        seen |= bit;
        if (id == CARPLAY_RESOURCE_MAIN_SCREEN) {
            entity = &next.screen_entity;
            permanent = &next.screen_permanent_entity;
            borrowed = &next.screen_borrowed;
        } else {
            entity = &next.audio_entity;
            permanent = &next.audio_permanent_entity;
            borrowed = &next.audio_borrowed;
        }
        const char *keys[] = {
            "transferPriority", "takeConstraint", "borrowConstraint", "unborrowConstraint"
        };
        uint64_t numbers[4] = {100, 100, 100, 100};
        int present[4];
        for (unsigned j = 0; j < 4; j++) {
            present[j] = optional_number(&doc, &resource, keys[j], &numbers[j]);
            if (present[j] < 0 || (present[j] && numbers[j] != 100 &&
                numbers[j] != 500 && (j == 0 || numbers[j] != 1000)))
                return modes_rejected(reason, keys[j]);
        }
        int *constraints = next.constraints[id - 1];
        if (transfer == 1) {
            if (!present[0] || !present[1] || !present[2] || present[3])
                return modes_rejected(reason, "Take requires priority/take/borrow constraints only");
            /* Incoming Take can replace our ownership only under our constraint.
             * This sender advertises Anytime initially and after relinquishing. */
            if (*permanent != CARPLAY_ENTITY_ACCESSORY &&
                (constraints[0] == 1000 || (constraints[0] == 500 && numbers[0] != 500)))
                return modes_rejected(reason, "Take blocked by current ownership constraint");
            *entity = *permanent = CARPLAY_ENTITY_ACCESSORY;
            *borrowed = 0;
            constraints[0] = (int)numbers[1];
            constraints[1] = (int)numbers[2];
            constraints[2] = 100;
        } else if (transfer == 2) {
            if (present[0] || present[1] || present[2] || present[3])
                return modes_rejected(reason, "Untake must not include priority or constraints");
            if (*permanent == CARPLAY_ENTITY_ACCESSORY) {
                *entity = *permanent = CARPLAY_ENTITY_NONE;
                *borrowed = 0;
                constraints[0] = constraints[1] = constraints[2] = 100;
            }
        } else if (transfer == 3) {
            if (present[1] || present[2])
                return modes_rejected(reason, "Borrow must not include take/borrow constraints");
            if (!*borrowed && *permanent != CARPLAY_ENTITY_ACCESSORY &&
                (constraints[1] == 1000 || (constraints[1] == 500 && numbers[0] != 500)))
                return modes_rejected(reason, "Borrow blocked by current ownership constraint");
            /* Also valid for audio permanently left to the car. Releasing
             * that borrow must never turn disabled audio into our resource.
             */
            *borrowed = 1;
            *entity = CARPLAY_ENTITY_ACCESSORY;
            constraints[2] = (int)numbers[3];
        } else {
            if (present[0] || present[1] || present[2] || present[3])
                return modes_rejected(reason, "Unborrow must not include priority or constraints");
            if (!*borrowed || *entity != CARPLAY_ENTITY_ACCESSORY)
                return modes_rejected(reason, "Unborrow has no active accessory borrow");
            *borrowed = 0;
            *entity = *permanent;
            constraints[2] = 100;
        }
    }
    *state = next;
    return 0;
}

int carplay_modes_apply(struct carplay_modes *state,
                        const uint8_t *body, size_t body_len)
{
    return carplay_modes_apply_with_reason(state, body, body_len, NULL);
}
