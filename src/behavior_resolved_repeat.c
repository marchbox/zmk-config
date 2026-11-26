// SPDX-License-Identifier: MIT

#define DT_DRV_COMPAT zmk_behavior_resolved_repeat

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct resolved_repeat_config {
    bool alternate;
    size_t alternates_len;
    const uint32_t *alternates;
};

struct resolved_repeat_data {
    uint32_t keycode;
    bool active;
};

static uint32_t last_keycode;
static bool replaying;

static uint8_t normalize_shift(uint8_t mods) {
    return mods & (MOD_LSFT | MOD_RSFT) ? (mods & ~MOD_RSFT) | MOD_LSFT : mods;
}

static uint32_t alternate_keycode(const struct resolved_repeat_config *config, uint32_t keycode) {
    uint8_t mods = SELECT_MODS(keycode);
    for (size_t i = 0; i < config->alternates_len; i += 2) {
        uint32_t source = config->alternates[i];
        uint8_t required = SELECT_MODS(source);
        if ((source & 0x00FFFFFF) != (keycode & 0x00FFFFFF) ||
            (normalize_shift(mods) & normalize_shift(required)) != normalize_shift(required)) {
            continue;
        }
        if (required & (MOD_LSFT | MOD_RSFT)) {
            required |= MOD_LSFT | MOD_RSFT;
        }
        return config->alternates[i + 1] | ((uint32_t)(mods & ~required) << 24);
    }
    return 0;
}

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
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct resolved_repeat_config *config = dev->config;
    struct resolved_repeat_data *data = dev->data;
    if (data->active) {
        LOG_ERR("Repeat is already held");
        return -EBUSY;
    }
    if (!last_keycode) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    uint32_t keycode =
        config->alternate ? alternate_keycode(config, last_keycode) : last_keycode;
    if (!keycode) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    data->keycode = keycode;
    data->active = true;
    replaying = true;
    int err = raise_zmk_keycode_state_changed_from_encoded(data->keycode, true, event.timestamp);
    replaying = false;
    if (err < 0) {
        LOG_ERR("Repeat press failed: %d", err);
        data->active = false;
    }
    return err;
}

static int resolved_repeat_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct resolved_repeat_data *data = dev->data;
    if (!data->active) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    replaying = true;
    int err = raise_zmk_keycode_state_changed_from_encoded(data->keycode, false, event.timestamp);
    replaying = false;
    data->active = false;
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
    BUILD_ASSERT(DT_INST_PROP_LEN(n, alternates) % 2 == 0, "Alternates must be keycode pairs");   \
    static const uint32_t alternates_##n[] = DT_INST_PROP(n, alternates);                          \
    static const struct resolved_repeat_config config_##n = {                                     \
        .alternate = DT_INST_PROP(n, alternate),                                                   \
        .alternates_len = ARRAY_SIZE(alternates_##n),                                              \
        .alternates = alternates_##n,                                                              \
    };                                                                                             \
    static struct resolved_repeat_data data_##n;                                                  \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &data_##n, &config_##n, POST_KERNEL,                     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &resolved_repeat_api);

DT_INST_FOREACH_STATUS_OKAY(REPEAT_INST)

#endif
