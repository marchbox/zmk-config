import unittest

from host_behavior import run_behavior_test

STUBS = r"""
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#define DT_HAS_COMPAT_STATUS_OKAY(...) 1
#define DT_NUM_INST_STATUS_OKAY(...) 1
#define DT_INST_FOREACH_STATUS_OKAY(...)
#define BUILD_ASSERT(condition, ...) _Static_assert(condition, "")
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...)
#define CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_CAPTURED_EVENTS 16
#define CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_HELD 8
#define ZMK_KEYMAP_LEN 34
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LS(key) ((key) | 0x02000000u)
#define MOD_LSFT 2
#define MOD_RSFT 32
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LISTENER(...)
#define ZMK_SUBSCRIPTION(...)
#define K_MSEC(ms) (ms)

struct device { const void *config; };
struct k_work {};
struct k_work_delayable {};
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1; };
struct zmk_behavior_binding_event { int layer; uint32_t position; int64_t timestamp; };
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};
struct zmk_position_state_changed { uint32_t position; bool state; int64_t timestamp; };
typedef struct zmk_position_state_changed zmk_event_t;
static const struct zmk_position_state_changed *
as_zmk_position_state_changed(const zmk_event_t *event) { return event; }
static int64_t now;
static int layer;
static uint8_t mods, masked_mods;
static uint32_t output;
static int presses, releases;
static struct device device;
static int64_t k_uptime_get(void) { return now; }
static int zmk_keymap_highest_layer_active(void) { return layer; }
static uint8_t zmk_hid_get_explicit_mods(void) { return mods; }
static void zmk_hid_masked_modifiers_set(uint8_t mask) { masked_mods = mask; }
static void zmk_hid_masked_modifiers_clear(void) { masked_mods = 0; }
static const struct device *zmk_behavior_get_binding(const char *name) { return &device; }
static void k_work_init_delayable(struct k_work_delayable *work,
                                  void (*handler)(struct k_work *)) {}
static int k_work_cancel_delayable(struct k_work_delayable *work) { return 0; }
static int k_work_reschedule(struct k_work_delayable *work, int64_t delay) { return 0; }
static int raise_zmk_keycode_state_changed_from_encoded(uint32_t key, bool pressed,
                                                        int64_t timestamp) {
    output = key;
    if (pressed) { presses++; } else { releases++; }
    return 0;
}
"""

HARNESS = r"""
#include <string.h>
#include "behavior_auto_shift.c"

static const uint32_t overrides[] = {1, 101, 2, 102, 3, 103};
static struct auto_shift_config config = {
    .tapping_term_ms = 175,
    .overrides_layer = 0,
    .overrides_len = ARRAY_SIZE(overrides),
    .overrides = overrides,
};
static struct zmk_behavior_binding binding = {.behavior_dev = "as"};
static struct zmk_behavior_binding_event event;

static void start(int active_layer, uint8_t modifiers, uint32_t key, bool combo) {
    memset(&pending, 0, sizeof(pending));
    memset(timing, 0, sizeof(timing));
    memset(history, 0, sizeof(history));
    memset(shortcuts, 0, sizeof(shortcuts));
    now = 1000;
    layer = active_layer;
    mods = modifiers;
    masked_mods = 0;
    presses = releases = 0;
    device.config = &config;
    binding.param1 = key;
    event = (struct zmk_behavior_binding_event){
        .position = combo ? ZMK_KEYMAP_LEN + 1 : 1,
        .timestamp = now,
    };
    assert(auto_shift_pressed(&binding, event) == 0);
}

static void expect_tap(uint32_t key) {
    assert(output == key);
    assert(presses == 1 && releases == 1);
    assert(masked_mods == 0);
}

int main(void) {
    for (int combo = 0; combo < 2; combo++) {
        for (uint32_t key = 1; key <= 4; key++) {
            for (int active_layer = 0; active_layer <= 1; active_layer++) {
                uint32_t shifted = active_layer == 0 && key <= 3 ? key + 100 : LS(key);

                start(active_layer, 0, key, combo);
                now += 100;
                assert(auto_shift_released(&binding, event) == 0);
                expect_tap(key);

                start(active_layer, 0, key, combo);
                now += 175;
                shift_timeout(NULL);
                expect_tap(shifted);
                assert(auto_shift_released(&binding, event) == 0);
                expect_tap(shifted);

                for (int right_shift = 0; right_shift < 2; right_shift++) {
                    start(active_layer, right_shift ? MOD_RSFT : MOD_LSFT, key, combo);
                    expect_tap(shifted);
                }

                start(active_layer, 0, key, combo);
                layer = 1 - active_layer;
                now += 200;
                assert(auto_shift_released(&binding, event) == 0);
                expect_tap(shifted);
            }
        }
    }

    // Hold-tap retro-taps resolve using the original physical duration.
    start(1, 0, 1, false);
    pending.active = false;
    timing[1] = (struct physical_timing){.press = 1000, .end = 1200};
    now = 1600;
    assert(auto_shift_pressed(&binding, event) == 0);
    expect_tap(LS(1));

    // Another key press resolves a held symbol using the press-time layer.
    start(1, 0, 2, false);
    layer = 0;
    struct zmk_position_state_changed other = {
        .position = 2, .state = true, .timestamp = 1200,
    };
    assert(auto_shift_position_listener(&other) == ZMK_EV_EVENT_BUBBLE);
    expect_tap(LS(2));

    // Existing configurations without a layer restriction retain their overrides.
    config.overrides_layer = -1;
    start(1, MOD_LSFT, 1, false);
    expect_tap(101);
    return 0;
}
"""


class AutoShiftTests(unittest.TestCase):
    def test_layer_specific_shift_outputs(self):
        headers = (
            "zephyr/device.h",
            "zephyr/kernel.h",
            "zephyr/logging/log.h",
            "drivers/behavior.h",
            "dt-bindings/zmk/keys.h",
            "zmk/behavior.h",
            "zmk/event_manager.h",
            "zmk/events/keycode_state_changed.h",
            "zmk/events/position_state_changed.h",
            "zmk/hid.h",
            "zmk/keymap.h",
            "zmk/matrix.h",
        )
        run_behavior_test(STUBS, HARNESS, headers)


if __name__ == "__main__":
    unittest.main()
