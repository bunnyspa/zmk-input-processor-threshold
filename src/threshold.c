#define DT_DRV_COMPAT zmk_input_processor_threshold

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

LOG_MODULE_REGISTER(threshold, CONFIG_ZMK_LOG_LEVEL);

static bool is_button(const struct input_event *event) {
    return event->type == INPUT_EV_KEY;
}

static bool is_scroll(const struct input_event *event) {
    return event->type == INPUT_EV_REL &&
           (event->code == INPUT_REL_WHEEL || event->code == INPUT_REL_HWHEEL);
}

struct threshold_config {
    uint32_t threshold;
    uint32_t window_ms;
    uint32_t wake_suppress_ms;
};

struct threshold_data {
    uint32_t accumulated; /* decaying sum of |dx|+|dy| */
    bool blocked;         /* true = blocking events until threshold is met */
    bool skip_frame;      /* true = threshold crossed mid-frame, drop rest of frame */
    int64_t last_event_ms;
    int64_t wake_recovery_until_ms;
};

static int threshold_handle_event(const struct device *dev,
                                  struct input_event *event,
                                  uint32_t param1,
                                  uint32_t param2,
                                  struct zmk_input_processor_state *state) {
    struct threshold_data *data = dev->data;
    const struct threshold_config *cfg = dev->config;

    /* param1/param2 override named properties when non-zero */
    uint32_t threshold = param1 ? param1 : cfg->threshold;
    uint32_t window_ms = param2 ? param2 : cfg->window_ms;

    int64_t now = k_uptime_get();

    /* Decay accumulated counts at rate threshold/window_ms.
     * Movement must sustain that rate to cross the threshold; once silent,
     * the accumulator drains and the device re-blocks. */
    int64_t dt = now - data->last_event_ms;
    if (dt > 0 && window_ms > 0) {
        int64_t decay = (dt * (int64_t)threshold) / (int64_t)window_ms;
        data->accumulated = decay >= (int64_t)data->accumulated
                                ? 0
                                : data->accumulated - (uint32_t)decay;
    }

    /* TODO: arm wake recovery from dt (sensor silent ≥ idle timeout) as a
     * fallback for when the burst arrives before ZMK_ACTIVITY_ACTIVE fires.
     * Threshold should use CONFIG_ZMK_IDLE_TIMEOUT rather than a hardcoded value.
    if (cfg->wake_suppress_ms > 0 && data->last_event_ms > 0 && dt >= CONFIG_ZMK_IDLE_TIMEOUT) {
        data->wake_recovery_until_ms = now + (int64_t)cfg->wake_suppress_ms;
        data->accumulated = 0;
        data->blocked = true;
        data->skip_frame = false;
        LOG_DBG("wake recovery armed by dt: %lldms", dt);
    } */

    data->last_event_ms = now;

    /* Discard X/Y events during wake recovery window to suppress sensor
     * recalibration noise that would otherwise falsely activate a temp layer. */
    if (data->wake_recovery_until_ms > 0 && now < data->wake_recovery_until_ms) {
        if (!event->sync &&
            event->type == INPUT_EV_REL &&
            (event->code == INPUT_REL_X || event->code == INPUT_REL_Y)) {
            data->accumulated = 0;
            data->blocked = true;
            data->skip_frame = false;
            LOG_WRN("wake suppress: val=%d remaining=%lldms",
                    event->value, data->wake_recovery_until_ms - now);
            return ZMK_INPUT_PROC_STOP;
        }
    }

    /* Drained back to zero while unblocked — re-arm the block. */
    if (data->accumulated == 0 && !data->blocked) {
        data->blocked = true;
        data->skip_frame = false;
        LOG_DBG("re-blocked (accumulator drained)");
    }

    /* Sync marks end-of-frame. Drop it while blocked or when threshold was crossed
     * mid-frame (skip_frame), so no partial frame leaks to downstream processors. */
    if (event->sync) {
        bool drop = data->blocked || data->skip_frame;
        data->skip_frame = false;
        return drop ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
    }

    /* Non-movement events do not contribute to accumulation */
    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        if (!data->blocked) return ZMK_INPUT_PROC_CONTINUE;
        if (is_button(event) && IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_THRESHOLD_BLOCK_BUTTONS)) return ZMK_INPUT_PROC_STOP;
        if (is_scroll(event) && IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_THRESHOLD_BLOCK_SCROLL))  return ZMK_INPUT_PROC_STOP;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* --- X/Y movement event handling below --- */

    /* Accumulate on every movement event — including while unblocked — so the
     * decay doesn't drain mid-gesture and re-block the user. Cap prevents
     * confident movement from banking unbounded credit toward re-block. */
    int32_t v = event->value;
    data->accumulated += (uint32_t)(v < 0 ? -v : v);
    uint32_t cap = threshold * 2;
    if (data->accumulated > cap) {
        data->accumulated = cap;
    }

    if (!data->blocked && !data->skip_frame) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (data->accumulated >= threshold) {
        /* Threshold met — allow events through, but skip the rest of this frame so
         * downstream processors see a clean full frame on the next cycle. */
        data->blocked = false;
        data->skip_frame = true;
        LOG_WRN("unblocked: accumulated=%u >= threshold=%u dt=%lldms val=%d",
                data->accumulated, threshold, dt, event->value);
    }

    /* Block further processing until threshold is met */
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api threshold_api = {
    .handle_event = threshold_handle_event,
};

#define THRESHOLD_INST(n)                                                    \
    static const struct threshold_config config_##n = {                      \
        .threshold       = DT_INST_PROP(n, threshold),                       \
        .window_ms       = DT_INST_PROP(n, window_ms),                       \
        .wake_suppress_ms = DT_INST_PROP(n, wake_suppress_ms),                 \
    };                                                                       \
    static struct threshold_data data_##n = {                                \
        .accumulated = 0,                                                    \
        .blocked = true,                                                     \
        .skip_frame = false,                                                 \
        .last_event_ms = 0,                                                  \
        .wake_recovery_until_ms = 0,                                         \
    };                                                                       \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &data_##n, &config_##n,             \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,  \
                          &threshold_api);

DT_INST_FOREACH_STATUS_OKAY(THRESHOLD_INST)

#define GET_THRESHOLD_DEV(n) DEVICE_DT_INST_GET(n),
static const struct device *threshold_devs[] = {
    DT_INST_FOREACH_STATUS_OKAY(GET_THRESHOLD_DEV)
};

static int on_activity_state(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev || ev->state != ZMK_ACTIVITY_ACTIVE) {
        return 0;
    }
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < ARRAY_SIZE(threshold_devs); i++) {
        const struct threshold_config *cfg = threshold_devs[i]->config;
        if (!cfg->wake_suppress_ms) {
            continue;
        }
        struct threshold_data *data = threshold_devs[i]->data;
        data->wake_recovery_until_ms = now + (int64_t)cfg->wake_suppress_ms;
        data->accumulated = 0;
        data->blocked = true;
        data->skip_frame = false;
        LOG_DBG("wake recovery armed: %ums", cfg->wake_suppress_ms);
    }
    return 0;
}

ZMK_LISTENER(threshold_activity_listener, on_activity_state);
ZMK_SUBSCRIPTION(threshold_activity_listener, zmk_activity_state_changed);
