import unittest

from host_behavior import run_behavior_test


STUBS = r"""
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#define DT_HAS_COMPAT_STATUS_OKAY(...) 1
#define DT_INST_FOREACH_STATUS_OKAY(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...)
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LISTENER(...)
#define ZMK_SUBSCRIPTION(...)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HID_USAGE_KEY 7
#define HID_USAGE_CONSUMER 12
#define ZMK_HID_USAGE(page, key) (((page) << 16) | (key))
#define SELECT_MODS(key) ((key) >> 24)
#define MOD_LSFT 2
#define MOD_RSFT 32
#define LS(key) ((key) | ((uint32_t)MOD_LSFT << 24))
#define RS(key) ((key) | ((uint32_t)MOD_RSFT << 24))
#define LC(key) ((key) | 0x01000000u)
#define LA(key) ((key) | 0x04000000u)
#define LG(key) ((key) | 0x08000000u)
#define LEFT ZMK_HID_USAGE(7, 0x50)
#define RIGHT ZMK_HID_USAGE(7, 0x4F)
#define UP ZMK_HID_USAGE(7, 0x52)
#define DOWN ZMK_HID_USAGE(7, 0x51)
#define J ZMK_HID_USAGE(7, 0x0D)
#define K ZMK_HID_USAGE(7, 0x0E)
#define A ZMK_HID_USAGE(7, 0x04)
#define C ZMK_HID_USAGE(7, 0x06)
#define V ZMK_HID_USAGE(7, 0x19)
#define X ZMK_HID_USAGE(7, 0x1B)

struct device { const void *config; void *data; };
struct zmk_behavior_binding { const char *behavior_dev; };
struct zmk_behavior_binding_event { int64_t timestamp; };
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};
struct zmk_keycode_state_changed {
    uint16_t usage_page;
    uint32_t keycode;
    bool state;
};
typedef struct zmk_keycode_state_changed zmk_event_t;
static const struct zmk_keycode_state_changed *
as_zmk_keycode_state_changed(const zmk_event_t *event) { return event; }
static bool is_mod(uint8_t page, uint32_t key) {
    return page == HID_USAGE_KEY && key >= 0xE0 && key <= 0xE7;
}
static struct { struct { uint8_t modifiers; } body; } report;
#define zmk_hid_get_keyboard_report() (&report)
static struct device normal_device, alternate_device;
static const struct device *zmk_behavior_get_binding(const char *name) {
    return !strcmp(name, "repeat") ? &normal_device : &alternate_device;
}
static int raise_zmk_keycode_state_changed_from_encoded(uint32_t key, bool pressed,
                                                        int64_t timestamp);
"""

HARNESS = r"""
#include "behavior_resolved_repeat.c"

static const uint32_t alternates[] = {
    LEFT, RIGHT, RIGHT, LEFT, UP, DOWN, DOWN, UP, LS(J), LS(K), LS(K), LS(J),
    LG(C), LG(LS(LA(V))), LG(X), LG(LS(LA(V))),
};
static const struct resolved_repeat_config normal_config;
static const struct resolved_repeat_config alternate_config = {
    .alternate = true,
    .alternates_len = ARRAY_SIZE(alternates),
    .alternates = alternates,
};
static struct resolved_repeat_data normal_data, alternate_data;
static struct zmk_behavior_binding normal = {.behavior_dev = "repeat"};
static struct zmk_behavior_binding alt = {.behavior_dev = "alt_repeat"};
static struct zmk_behavior_binding_event event = {.timestamp = 1000};
static struct { uint32_t key; bool pressed; } outputs[64];
static size_t output_count;
static int output_error;

static int raise_zmk_keycode_state_changed_from_encoded(uint32_t key, bool pressed,
                                                        int64_t timestamp) {
    if (output_error) { return output_error; }
    assert(output_count < ARRAY_SIZE(outputs));
    outputs[output_count].key = key;
    outputs[output_count++].pressed = pressed;
    struct zmk_keycode_state_changed output = {
        .usage_page = (key >> 16) & 0xFF, .keycode = key & 0xFFFF, .state = pressed,
    };
    report.body.modifiers = SELECT_MODS(key);
    resolved_repeat_listener(&output);
    return 0;
}

static void send_key_state(uint32_t key, bool pressed) {
    struct zmk_keycode_state_changed input = {
        .usage_page = (key >> 16) & 0xFF, .keycode = key & 0xFFFF, .state = pressed,
    };
    report.body.modifiers = SELECT_MODS(key);
    resolved_repeat_listener(&input);
}

static void remember(uint32_t key) { send_key_state(key, true); }

static void tap(struct zmk_behavior_binding *binding, uint32_t expected) {
    size_t before = output_count;
    assert(resolved_repeat_pressed(binding, event) == 0);
    assert(resolved_repeat_released(binding, event) == 0);
    assert(output_count == before + (expected ? 2 : 0));
    if (expected) {
        assert(outputs[before].key == expected && outputs[before].pressed);
        assert(outputs[before + 1].key == expected && !outputs[before + 1].pressed);
    }
}

int main(void) {
    normal_device = (struct device){.config = &normal_config, .data = &normal_data};
    alternate_device = (struct device){.config = &alternate_config, .data = &alternate_data};
    tap(&normal, 0);
    tap(&alt, 0);

    for (size_t i = 0; i < ARRAY_SIZE(alternates); i += 2) {
        output_count = 0;
        remember(alternates[i]);
        tap(&alt, alternates[i + 1]);
        tap(&alt, alternates[i + 1]);
        tap(&normal, alternates[i]);
    }
    remember(J);
    tap(&alt, 0);
    remember(A);
    tap(&alt, 0);
    tap(&normal, A);

    output_count = 0;
    remember(LC(LS(LEFT)));
    tap(&alt, LC(LS(RIGHT)));
    remember(LC(RS(J)));
    tap(&alt, LC(LS(K)));
    remember(RS(K));
    tap(&alt, LS(J));

    // Modifiers and unrelated usage pages must not overwrite the remembered key.
    remember(ZMK_HID_USAGE(7, 0xE1));
    remember(ZMK_HID_USAGE(1, 0x30));
    tap(&alt, LS(J));
    remember(ZMK_HID_USAGE(12, 0xE9));
    tap(&normal, ZMK_HID_USAGE(12, 0xE9));
    tap(&alt, 0);

    // Copy/cut survive modifier changes and non-keyboard/non-consumer input.
    // ZMK layer switches and mouse actions do not emit keycode events at all.
    const uint32_t clipboard_keys[] = {LG(C), LG(X)};
    for (size_t i = 0; i < ARRAY_SIZE(clipboard_keys); i++) {
        output_count = 0;
        remember(clipboard_keys[i]);
        send_key_state(clipboard_keys[i] & 0x00FFFFFF, false);
        for (uint32_t modifier = 0xE0; modifier <= 0xE7; modifier++) {
            send_key_state(ZMK_HID_USAGE(7, modifier), true);
            send_key_state(ZMK_HID_USAGE(7, modifier), false);
        }
        remember(ZMK_HID_USAGE(1, 0x30));
        remember(ZMK_HID_USAGE(1, 0x38));
        remember(ZMK_HID_USAGE(9, 1));
        report.body.modifiers = 0;
        tap(&alt, LG(LS(LA(V))));
        tap(&alt, LG(LS(LA(V))));
        tap(&normal, clipboard_keys[i]);
        remember(A);
        tap(&alt, 0);
    }

    // Both repeat keys can be held, and each releases its own press-time key.
    output_count = 0;
    remember(LEFT);
    assert(resolved_repeat_pressed(&normal, event) == 0);
    assert(resolved_repeat_pressed(&alt, event) == 0);
    assert(resolved_repeat_pressed(&alt, event) == -EBUSY);
    remember(UP);
    assert(resolved_repeat_released(&normal, event) == 0);
    assert(resolved_repeat_released(&alt, event) == 0);
    assert(outputs[2].key == LEFT && !outputs[2].pressed);
    assert(outputs[3].key == RIGHT && !outputs[3].pressed);
    tap(&alt, DOWN);

    // A failed press must not leave the behavior active or emit a release.
    output_error = -EIO;
    assert(resolved_repeat_pressed(&alt, event) == -EIO);
    assert(!alternate_data.active);
    size_t before = output_count;
    assert(resolved_repeat_released(&alt, event) == 0);
    assert(output_count == before);
    output_error = 0;
    tap(&alt, DOWN);
    return 0;
}
"""


class ResolvedRepeatTests(unittest.TestCase):
    def test_alternate_mappings_and_repeat_lifecycle(self):
        run_behavior_test(
            STUBS,
            HARNESS,
            (
                "zephyr/device.h",
                "zephyr/logging/log.h",
                "drivers/behavior.h",
                "zmk/behavior.h",
                "zmk/event_manager.h",
                "zmk/events/keycode_state_changed.h",
                "zmk/hid.h",
            ),
        )


if __name__ == "__main__":
    unittest.main()
