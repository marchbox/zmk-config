// SPDX-License-Identifier: MIT

#define DT_DRV_COMPAT zmk_behavior_auto_shift

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct auto_shift_config {
    int tapping_term_ms;
    int overrides_layer;
    size_t overrides_len;
    const uint32_t *overrides;
};

struct physical_timing {
    int64_t press;
    int64_t end;
    bool pressed;
};

static struct physical_timing timing[ZMK_KEYMAP_LEN];
static struct {
    uint32_t position;
    struct physical_timing timing;
} history[CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_CAPTURED_EVENTS];
static size_t history_next;
static struct {
    uint32_t position;
    uint32_t keycode;
    bool active;
} shortcuts[CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_HELD];
static struct k_work_delayable shift_work;
static struct {
    const struct auto_shift_config *config;
    struct zmk_behavior_binding_event event;
    uint32_t keycode;
    uint32_t shifted_keycode;
    bool active;
} pending;

static uint32_t shifted_key(const struct auto_shift_config *config, uint32_t keycode) {
    if (config->overrides_layer >= 0 &&
        zmk_keymap_highest_layer_active() != config->overrides_layer) {
        return LS(keycode);
    }
    for (size_t i = 0; i < config->overrides_len; i += 2) {
        if (config->overrides[i] == keycode) {
            return config->overrides[i + 1];
        }
    }
    return LS(keycode);
}

static int emit_tap(uint32_t keycode, bool mask_shift, int64_t timestamp) {
    if (mask_shift) {
        zmk_hid_masked_modifiers_set(MOD_LSFT | MOD_RSFT);
    }
    int err = raise_zmk_keycode_state_changed_from_encoded(keycode, true, timestamp);
    int release_err = raise_zmk_keycode_state_changed_from_encoded(keycode, false, timestamp);
    if (mask_shift) {
        zmk_hid_masked_modifiers_clear();
    }
    if (err < 0 || release_err < 0) {
        LOG_ERR("Auto Shift output failed: %d / %d", err, release_err);
    }
    return err < 0 ? err : release_err;
}

static int finish_pending(int64_t timestamp) {
    if (!pending.active) {
        return 0;
    }
    pending.active = false;
    k_work_cancel_delayable(&shift_work);
    bool shifted = timestamp - pending.event.timestamp >= pending.config->tapping_term_ms;
    uint32_t keycode = shifted ? pending.shifted_keycode : pending.keycode;
    return emit_tap(keycode, shifted, pending.event.timestamp);
}

static void shift_timeout(struct k_work *work) { finish_pending(k_uptime_get()); }

static int auto_shift_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct auto_shift_config *config = dev->config;
    int err = finish_pending(event.timestamp);
    if (err < 0) {
        return err;
    }

    uint8_t mods = zmk_hid_get_explicit_mods();
    if (mods & (MOD_LSFT | MOD_RSFT)) {
        return emit_tap(shifted_key(config, binding->param1), true, event.timestamp);
    }
    if (mods) {
        for (size_t i = 0; i < ARRAY_SIZE(shortcuts); i++) {
            if (!shortcuts[i].active) {
                shortcuts[i].position = event.position;
                shortcuts[i].keycode = binding->param1;
                err = raise_zmk_keycode_state_changed_from_encoded(binding->param1, true,
                                                                   event.timestamp);
                shortcuts[i].active = err >= 0;
                return err;
            }
        }
        LOG_ERR("No free Auto Shift shortcut slot");
        return -ENOMEM;
    }

    pending.config = config;
    if (event.position >= ARRAY_SIZE(timing)) {
        event.timestamp = k_uptime_get();
    }
    pending.event = event;
    pending.keycode = binding->param1;
    // Keep the press-time layer's output even if the layer changes before resolution.
    pending.shifted_keycode = shifted_key(config, binding->param1);
    pending.active = true;

    // Hold-tap supplies the original press timestamp even on a retro-tap release.
    if (event.position < ARRAY_SIZE(timing) && timing[event.position].press == event.timestamp &&
        timing[event.position].end) {
        return finish_pending(timing[event.position].end);
    }
    for (size_t i = 0; i < ARRAY_SIZE(history); i++) {
        if (history[i].position == event.position && history[i].timing.press == event.timestamp &&
            history[i].timing.end) {
            return finish_pending(history[i].timing.end);
        }
    }
    int64_t remaining = event.timestamp + config->tapping_term_ms - k_uptime_get();
    if (remaining <= 0) {
        return finish_pending(k_uptime_get());
    }
    k_work_reschedule(&shift_work, K_MSEC(remaining));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int auto_shift_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    if (pending.active && pending.event.position == event.position) {
        return finish_pending(k_uptime_get());
    }
    // Delayed Auto Shift outputs are taps; modifier shortcuts remain held.
    for (size_t i = 0; i < ARRAY_SIZE(shortcuts); i++) {
        if (shortcuts[i].active && shortcuts[i].position == event.position) {
            shortcuts[i].active = false;
            return raise_zmk_keycode_state_changed_from_encoded(shortcuts[i].keycode, false,
                                                                k_uptime_get());
        }
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int auto_shift_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);
    if (!event) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (event->state) {
        for (size_t i = 0; i < ARRAY_SIZE(timing); i++) {
            if (i != event->position && timing[i].pressed && !timing[i].end &&
                event->timestamp >= timing[i].press) {
                timing[i].end = event->timestamp;
            }
        }
        if (pending.active && pending.event.position != event->position) {
            finish_pending(event->timestamp);
        }
    }
    if (event->position < ARRAY_SIZE(timing)) {
        struct physical_timing *key = &timing[event->position];
        if (event->state) {
            // Replayed captured events must not reset the physical timing.
            if (key->press != event->timestamp) {
                if (key->end) {
                    history[history_next].position = event->position;
                    history[history_next].timing = *key;
                    history_next = (history_next + 1) % ARRAY_SIZE(history);
                }
                *key = (struct physical_timing){.press = event->timestamp, .pressed = true};
            }
        } else {
            key->pressed = false;
            if (!key->end) {
                key->end = event->timestamp;
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(auto_shift, auto_shift_position_listener);
ZMK_SUBSCRIPTION(auto_shift, zmk_position_state_changed);

static int auto_shift_init(const struct device *dev) {
    k_work_init_delayable(&shift_work, shift_timeout);
    return 0;
}

static const struct behavior_driver_api auto_shift_api = {
    .binding_pressed = auto_shift_pressed,
    .binding_released = auto_shift_released,
};

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Auto Shift uses one shared override table");

#define AS_INST(n)                                                                                 \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, overrides) % 2 == 0, "Overrides must be keycode pairs");      \
    static const uint32_t overrides_##n[] = DT_INST_PROP(n, overrides);                            \
    static const struct auto_shift_config config_##n = {                                           \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .overrides_layer = DT_INST_PROP(n, overrides_layer),                                       \
        .overrides_len = ARRAY_SIZE(overrides_##n),                                                \
        .overrides = overrides_##n,                                                                \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, auto_shift_init, NULL, NULL, &config_##n, POST_KERNEL,              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &auto_shift_api);

DT_INST_FOREACH_STATUS_OKAY(AS_INST)

#endif
