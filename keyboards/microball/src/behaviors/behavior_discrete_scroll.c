/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT microball_behavior_discrete_scroll

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct discrete_scroll_config {
    int16_t cw_delta;
    int16_t ccw_delta;
};

struct discrete_scroll_data {
    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
};

static int discrete_scroll_accept_data(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event,
                                       const struct zmk_sensor_config *sensor_config,
                                       size_t channel_data_size,
                                       const struct zmk_sensor_channel_data *channel_data) {
    ARG_UNUSED(binding);
    ARG_UNUSED(channel_data_size);

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct discrete_scroll_data *data = dev->data;

    const struct sensor_value value = channel_data[0].value;
    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int triggers;

    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder = data->remainder[sensor_index][event.layer];

        remainder.val1 += value.val1;
        remainder.val2 += value.val2;

        if (remainder.val2 >= 1000000 || remainder.val2 <= -1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }

        const int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;

        data->remainder[sensor_index][event.layer] = remainder;
    }

    data->triggers[sensor_index][event.layer] = triggers;
    return 0;
}

static int discrete_scroll_process(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event,
                                   enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct discrete_scroll_config *cfg = dev->config;
    struct discrete_scroll_data *data = dev->data;

    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers[sensor_index][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    const int triggers = data->triggers[sensor_index][event.layer];
    data->triggers[sensor_index][event.layer] = 0;

    if (triggers == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    const int16_t delta = (triggers > 0) ? cfg->cw_delta : cfg->ccw_delta;
    const int16_t amount = delta * ABS(triggers);

    LOG_DBG("Reporting wheel amount %d for %d trigger(s)", amount, triggers);

    return input_report_rel(dev, INPUT_REL_WHEEL, amount, true, K_NO_WAIT);
}

static const struct behavior_driver_api discrete_scroll_driver_api = {
    .sensor_binding_accept_data = discrete_scroll_accept_data,
    .sensor_binding_process = discrete_scroll_process,
};

#define DISCRETE_SCROLL_INST(n)                                                                   \
    static struct discrete_scroll_data discrete_scroll_data_##n = {};                             \
    static const struct discrete_scroll_config discrete_scroll_config_##n = {                     \
        .cw_delta = DT_INST_PROP_OR(n, cw_delta, -1),                                            \
        .ccw_delta = DT_INST_PROP_OR(n, ccw_delta, 1),                                           \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &discrete_scroll_data_##n,                            \
                            &discrete_scroll_config_##n, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &discrete_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DISCRETE_SCROLL_INST)
