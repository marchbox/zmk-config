// SPDX-License-Identifier: MIT

#define DT_DRV_COMPAT zmk_behavior_resolved_repeat

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static uint32_t last_keycode;
static uint32_t repeat_keycode;
static bool repeating;
static bool replaying;

static int resolved_repeat_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *event = as_zmk_keycode_state_changed(eh);
    if (event && event->state && !replaying &&
        (event->usage_page == HID_USAGE_KEY || event->usage_page == HID_USAGE_CONSUMER) &&
        !is_mod(event->usage_page, event->keycode)) {
        uint8_t mods = zmk_hid_get_keyboard_report()->body.modifiers;
        last_keycode = ZMK_HID_USAGE(event->usage_page, event->keycode) | ((uint32_t)mods << 24);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(resolved_repeat, resolved_repeat_listener);
ZMK_SUBSCRIPTION(resolved_repeat, zmk_keycode_state_changed);

static int resolved_repeat_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    if (!last_keycode) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    repeat_keycode = last_keycode;
    repeating = true;
    replaying = true;
    int err = raise_zmk_keycode_state_changed_from_encoded(repeat_keycode, true, event.timestamp);
    replaying = false;
    if (err < 0) {
        LOG_ERR("Repeat press failed: %d", err);
        repeating = false;
    }
    return err;
}

static int resolved_repeat_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    if (!repeating) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    replaying = true;
    int err = raise_zmk_keycode_state_changed_from_encoded(repeat_keycode, false, event.timestamp);
    replaying = false;
    repeating = false;
    if (err < 0) {
        LOG_ERR("Repeat release failed: %d", err);
    }
    return err;
}

static const struct behavior_driver_api resolved_repeat_api = {
    .binding_pressed = resolved_repeat_pressed,
    .binding_released = resolved_repeat_released,
};

#define REPEAT_INST(n)                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &resolved_repeat_api);

DT_INST_FOREACH_STATUS_OKAY(REPEAT_INST)

#endif
