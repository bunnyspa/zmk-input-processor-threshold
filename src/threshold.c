#define DT_DRV_COMPAT zmk_input_processor_threshold

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(threshold, CONFIG_ZMK_LOG_LEVEL);

static bool is_button(const struct input_event *event) {
    return event->type == INPUT_EV_KEY;
}

static bool is_scroll(const struct input_event *event) {
    return event->type == INPUT_EV_REL &&
           (event->code == INPUT_REL_WHEEL || event->code == INPUT_REL_HWHEEL);
}

struct threshold_data {
    uint32_t accumulated; /* decaying sum of |dx|+|dy| */
    bool blocked;         /* true = blocking events until threshold is met */
    bool skip_frame;      /* true = threshold crossed mid-frame, drop rest of frame */
    int64_t last_event_ms;
};

static int threshold_handle_event(const struct device *dev,
                                  struct input_event *event,
                                  uint32_t param1,
                                  uint32_t param2,
                                  struct zmk_input_processor_state *state) {
    struct threshold_data *data = dev->data;
    uint32_t threshold = param1;
    uint32_t window_ms = param2;

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

    /* Log the first event after a long gap — likely a wake-up burst from the sensor. */
    bool is_wake_event = (data->last_event_ms == 0 || dt > 500);
    if (is_wake_event && !event->sync &&
        event->type == INPUT_EV_REL &&
        (event->code == INPUT_REL_X || event->code == INPUT_REL_Y)) {
        LOG_WRN("wake burst: dt=%lldms code=%u val=%d acc_before=%u blocked=%d",
                dt, event->code, event->value, data->accumulated, data->blocked);
    }

    data->last_event_ms = now;

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
        LOG_WRN("unblocked: accumulated=%u >= threshold=%u dt_since_last=%lldms val=%d code=%u",
                data->accumulated, threshold, dt, event->value, event->code);
    }

    /* Block further processing until threshold is met */
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api threshold_api = {
    .handle_event = threshold_handle_event,
};

#define THRESHOLD_INST(n)                                                   \
    static struct threshold_data data_##n = {                               \
        .accumulated = 0,                                                   \
        .blocked = true,                                                    \
        .skip_frame = false,                                                \
        .last_event_ms = 0,                                                 \
    };                                                                      \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &data_##n, NULL,                   \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                          &threshold_api);

DT_INST_FOREACH_STATUS_OKAY(THRESHOLD_INST)
