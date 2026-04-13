/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_tap_dance_triple

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD CONFIG_ZMK_BEHAVIOR_TAP_DANCE_TRIPLE_MAX_HELD
#define ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE UINT32_MAX

enum tdt_binding_index {
    TDT_SINGLE_TAP = 0,
    TDT_SINGLE_HOLD = 1,
    TDT_DOUBLE_TAP = 2,
};

enum tdt_state {
    TDT_STATE_IDLE = 0,
    TDT_STATE_FIRST_DOWN,
    TDT_STATE_WAIT_SECOND,
    TDT_STATE_SECOND_DOWN,
    TDT_STATE_SINGLE_HOLD,
};

struct behavior_tap_dance_triple_config {
    uint32_t tapping_term_ms;
    size_t behavior_count;
    struct zmk_behavior_binding *behaviors;
};

struct active_tap_dance_triple {
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    bool is_pressed;
    bool timer_cancelled;
    int64_t deadline;
    enum tdt_state state;
    const struct behavior_tap_dance_triple_config *config;
    struct k_work_delayable timer;
};

static struct active_tap_dance_triple active_tap_dances[ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD] = {};

static struct active_tap_dance_triple *find_tap_dance(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD; i++) {
        if (active_tap_dances[i].position == position) {
            return &active_tap_dances[i];
        }
    }
    return NULL;
}

static void clear_tap_dance(struct active_tap_dance_triple *tap_dance) {
    tap_dance->position = ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE;
    tap_dance->state = TDT_STATE_IDLE;
    tap_dance->is_pressed = false;
    tap_dance->timer_cancelled = false;
    tap_dance->deadline = 0;
    tap_dance->config = NULL;
}

static int new_tap_dance(struct zmk_behavior_binding_event *event,
                         const struct behavior_tap_dance_triple_config *config,
                         struct active_tap_dance_triple **tap_dance) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD; i++) {
        struct active_tap_dance_triple *ref = &active_tap_dances[i];
        if (ref->position == ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE) {
            ref->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            ref->source = event->source;
#endif
            ref->is_pressed = true;
            ref->timer_cancelled = false;
            ref->deadline = 0;
            ref->state = TDT_STATE_FIRST_DOWN;
            ref->config = config;
            *tap_dance = ref;
            return 0;
        }
    }
    return -ENOMEM;
}

static int stop_timer(struct active_tap_dance_triple *tap_dance) {
    int ret = k_work_cancel_delayable(&tap_dance->timer);
    if (ret == -EINPROGRESS) {
        tap_dance->timer_cancelled = true;
    }
    return ret;
}

static void schedule_timer(struct active_tap_dance_triple *tap_dance, int64_t timestamp) {
    tap_dance->deadline = timestamp + tap_dance->config->tapping_term_ms;
    tap_dance->timer_cancelled = false;

    int32_t ms_left = tap_dance->deadline - k_uptime_get();
    if (ms_left <= 0) {
        ms_left = 1;
    }

    k_work_schedule(&tap_dance->timer, K_MSEC(ms_left));
}

static struct zmk_behavior_binding get_binding(const struct active_tap_dance_triple *tap_dance,
                                               enum tdt_binding_index index) {
    size_t count = tap_dance->config->behavior_count;
    if (count == 0) {
        static struct zmk_behavior_binding empty = {
            .behavior_dev = "trans",
            .param1 = 0,
            .param2 = 0,
        };
        return empty;
    }

    size_t selected = index < count ? index : (count - 1);
    return tap_dance->config->behaviors[selected];
}

static int invoke_binding(struct active_tap_dance_triple *tap_dance, enum tdt_binding_index index,
                          int64_t timestamp, bool pressed) {
    struct zmk_behavior_binding binding = get_binding(tap_dance, index);
    struct zmk_behavior_binding_event event = {
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };

    return zmk_behavior_invoke_binding(&binding, event, pressed);
}

static int tap_binding(struct active_tap_dance_triple *tap_dance, enum tdt_binding_index index,
                       int64_t timestamp) {
    int ret = invoke_binding(tap_dance, index, timestamp, true);
    if (ret < 0) {
        return ret;
    }

    return invoke_binding(tap_dance, index, timestamp, false);
}

static void decide_single_hold(struct active_tap_dance_triple *tap_dance, int64_t timestamp) {
    invoke_binding(tap_dance, TDT_SINGLE_HOLD, timestamp, true);
    tap_dance->state = TDT_STATE_SINGLE_HOLD;
}

static void decide_single_tap(struct active_tap_dance_triple *tap_dance, int64_t timestamp) {
    tap_binding(tap_dance, TDT_SINGLE_TAP, timestamp);
    clear_tap_dance(tap_dance);
}

static void decide_double_tap(struct active_tap_dance_triple *tap_dance, int64_t timestamp) {
    tap_binding(tap_dance, TDT_DOUBLE_TAP, timestamp);
    clear_tap_dance(tap_dance);
}

static void decide_expired_state_if_needed(struct active_tap_dance_triple *tap_dance,
                                           int64_t timestamp) {
    // If the timer callback hasn't run yet, release still needs to observe the expired outcome.
    if (tap_dance->deadline == 0 || timestamp < tap_dance->deadline) {
        return;
    }

    switch (tap_dance->state) {
    case TDT_STATE_FIRST_DOWN:
        stop_timer(tap_dance);
        decide_single_hold(tap_dance, tap_dance->deadline);
        break;
    case TDT_STATE_SECOND_DOWN:
        stop_timer(tap_dance);
        decide_double_tap(tap_dance, tap_dance->deadline);
        break;
    default:
        break;
    }
}

static int on_tap_dance_triple_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tap_dance_triple_config *cfg = dev->config;
    struct active_tap_dance_triple *tap_dance = find_tap_dance(event.position);

    if (tap_dance == NULL) {
        if (new_tap_dance(&event, cfg, &tap_dance) == -ENOMEM) {
            LOG_ERR("Unable to allocate active triple tap dance");
            return ZMK_BEHAVIOR_OPAQUE;
        }

        schedule_timer(tap_dance, event.timestamp);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    tap_dance->is_pressed = true;
    if (tap_dance->state == TDT_STATE_WAIT_SECOND && event.timestamp >= tap_dance->deadline) {
        decide_single_tap(tap_dance, tap_dance->deadline);
        tap_dance = NULL;
    } else {
        stop_timer(tap_dance);
    }

    if (tap_dance == NULL) {
        if (new_tap_dance(&event, cfg, &tap_dance) == -ENOMEM) {
            LOG_ERR("Unable to allocate active triple tap dance");
            return ZMK_BEHAVIOR_OPAQUE;
        }

        schedule_timer(tap_dance, event.timestamp);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (tap_dance->state == TDT_STATE_WAIT_SECOND) {
        tap_dance->state = TDT_STATE_SECOND_DOWN;
        schedule_timer(tap_dance, event.timestamp);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tap_dance_triple_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    struct active_tap_dance_triple *tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    decide_expired_state_if_needed(tap_dance, event.timestamp);
    if (tap_dance->position == ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    tap_dance->is_pressed = false;

    switch (tap_dance->state) {
    case TDT_STATE_FIRST_DOWN:
        stop_timer(tap_dance);
        tap_dance->state = TDT_STATE_WAIT_SECOND;
        schedule_timer(tap_dance, event.timestamp);
        break;
    case TDT_STATE_SECOND_DOWN:
        stop_timer(tap_dance);
        decide_double_tap(tap_dance, event.timestamp);
        break;
    case TDT_STATE_SINGLE_HOLD:
        invoke_binding(tap_dance, TDT_SINGLE_HOLD, event.timestamp, false);
        clear_tap_dance(tap_dance);
        break;
    default:
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static void behavior_tap_dance_triple_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tap_dance_triple *tap_dance =
        CONTAINER_OF(d_work, struct active_tap_dance_triple, timer);

    if (tap_dance->position == ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE ||
        tap_dance->timer_cancelled) {
        return;
    }

    if (k_uptime_get() < tap_dance->deadline) {
        return;
    }

    switch (tap_dance->state) {
    case TDT_STATE_FIRST_DOWN:
        if (tap_dance->is_pressed) {
            decide_single_hold(tap_dance, tap_dance->deadline);
        }
        break;
    case TDT_STATE_WAIT_SECOND:
        if (!tap_dance->is_pressed) {
            decide_single_tap(tap_dance, tap_dance->deadline);
        }
        break;
    case TDT_STATE_SECOND_DOWN:
        if (tap_dance->is_pressed) {
            decide_double_tap(tap_dance, tap_dance->deadline);
        }
        break;
    default:
        break;
    }
}

static const struct behavior_driver_api behavior_tap_dance_triple_driver_api = {
    .binding_pressed = on_tap_dance_triple_binding_pressed,
    .binding_released = on_tap_dance_triple_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int tap_dance_triple_position_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_tap_dance_triple, tap_dance_triple_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_tap_dance_triple, zmk_position_state_changed);

static int tap_dance_triple_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD; i++) {
        struct active_tap_dance_triple *tap_dance = &active_tap_dances[i];
        if (tap_dance->position == ZMK_BHV_TAP_DANCE_TRIPLE_POSITION_FREE ||
            tap_dance->position == ev->position) {
            continue;
        }

        // QMK-like interruption: another key resolves the pending dance immediately.
        switch (tap_dance->state) {
        case TDT_STATE_FIRST_DOWN:
            stop_timer(tap_dance);
            decide_single_hold(tap_dance, ev->timestamp);
            break;
        case TDT_STATE_WAIT_SECOND:
            stop_timer(tap_dance);
            decide_single_tap(tap_dance, ev->timestamp);
            break;
        case TDT_STATE_SECOND_DOWN:
            stop_timer(tap_dance);
            decide_double_tap(tap_dance, ev->timestamp);
            break;
        default:
            break;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_tap_dance_triple_init(const struct device *dev) {
    ARG_UNUSED(dev);

    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_TAP_DANCE_TRIPLE_MAX_HELD; i++) {
            k_work_init_delayable(&active_tap_dances[i].timer,
                                  behavior_tap_dance_triple_timer_handler);
            clear_tap_dance(&active_tap_dances[i]);
        }
        init_first_run = false;
    }

    return 0;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node)

#define TRANSFORMED_BINDINGS(node)                                                                  \
    { LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, (, ), DT_DRV_INST(node)) }

#define TDT_INST(n)                                                                                \
    static struct zmk_behavior_binding                                                             \
        behavior_tap_dance_triple_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =          \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_tap_dance_triple_config behavior_tap_dance_triple_config_##n = {       \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .behaviors = behavior_tap_dance_triple_config_##n##_bindings,                              \
        .behavior_count = DT_INST_PROP_LEN(n, bindings),                                           \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tap_dance_triple_init, NULL, NULL,                         \
                            &behavior_tap_dance_triple_config_##n, POST_KERNEL,                    \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_tap_dance_triple_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TDT_INST)

#endif
