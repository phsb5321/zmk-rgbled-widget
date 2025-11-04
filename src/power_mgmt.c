/*
 * RGB LED Power Management Module
 * Provides intelligent power control for RGB LEDs on battery-powered keyboards
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT)

#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk_rgbled_widget/power_mgmt.h>

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

LOG_MODULE_REGISTER(rgbled_power_mgmt, CONFIG_ZMK_LOG_LEVEL);

/* Internal state structure */
struct power_mgmt_state {
    enum rgbled_power_state current_state;
    struct rgbled_power_config config;
    struct rgbled_power_status status;
    bool initialized;
    bool override_active;
    bool external_power_available;
    const struct device *ext_power_dev;
    int64_t last_activity_time;
    int64_t startup_time;
    int64_t last_evaluation_time;
    struct k_work_delayable evaluation_work;
    struct k_work_delayable startup_work;
    rgbled_power_state_callback_t callbacks[RGBLED_POWER_MAX_CALLBACKS];
    void *callback_user_data[RGBLED_POWER_MAX_CALLBACKS];
    uint8_t callback_count;
};

static struct power_mgmt_state pm_state = {
    .current_state = RGBLED_POWER_INITIALIZING,
    .initialized = false,
    .override_active = false,
    .external_power_available = false,
    .ext_power_dev = NULL,
    .callback_count = 0,
};

/* Forward declarations */
static void power_evaluation_work_handler(struct k_work *work);
static void startup_delay_work_handler(struct k_work *work);
static void set_power_state(enum rgbled_power_state new_state, enum rgbled_power_trigger trigger);
static bool should_power_off(void);
static bool should_power_on(void);
static void notify_callbacks(enum rgbled_power_state old_state, enum rgbled_power_state new_state, enum rgbled_power_trigger trigger);

/* Initialize default configuration from Kconfig */
static void init_default_config(void) {
    pm_state.config.enabled = true;
    pm_state.config.battery_threshold = CONFIG_RGBLED_WIDGET_POWER_MGMT_BATTERY_THRESHOLD;
    pm_state.config.battery_hysteresis = CONFIG_RGBLED_WIDGET_POWER_MGMT_HYSTERESIS;
    pm_state.config.idle_timeout_ms = CONFIG_RGBLED_WIDGET_POWER_MGMT_IDLE_TIMEOUT_MS;
    pm_state.config.startup_delay_ms = CONFIG_RGBLED_WIDGET_POWER_MGMT_STARTUP_DELAY_MS;
    
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT_ENABLE_ACTIVITY_DETECTION)
    pm_state.config.activity_detection_enabled = true;
#else
    pm_state.config.activity_detection_enabled = false;
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT_ENABLE_BATTERY_DETECTION)
    pm_state.config.battery_detection_enabled = true;
#else
    pm_state.config.battery_detection_enabled = false;
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT_DEBUG)
    pm_state.config.debug_enabled = true;
#else
    pm_state.config.debug_enabled = false;
#endif
}

/* External power control */
static int control_external_power(bool enable) {
#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
    if (!pm_state.external_power_available || pm_state.ext_power_dev == NULL) {
        return -ENOTSUP;
    }
    
    int ret = enable ? ext_power_enable(pm_state.ext_power_dev) : ext_power_disable(pm_state.ext_power_dev);
    if (ret == 0) {
        LOG_DBG("External power %s", enable ? "enabled" : "disabled");
    } else {
        LOG_ERR("Failed to %s external power: %d", enable ? "enable" : "disable", ret);
    }
    return ret;
#else
    LOG_WRN("External power control not available");
    return -ENOTSUP;
#endif
}

/* State machine implementation */
static void set_power_state(enum rgbled_power_state new_state, enum rgbled_power_trigger trigger) {
    enum rgbled_power_state old_state = pm_state.current_state;
    
    if (old_state == new_state) {
        return;
    }
    
    pm_state.current_state = new_state;
    pm_state.status.state = new_state;
    pm_state.status.last_trigger = trigger;
    pm_state.status.last_state_change_time = k_uptime_get();
    pm_state.status.state_change_count++;
    
    /* Control external power based on state */
    switch (new_state) {
        case RGBLED_POWER_ON:
        case RGBLED_POWER_EVALUATING_OFF:
            control_external_power(true);
            break;
        case RGBLED_POWER_OFF:
        case RGBLED_POWER_EVALUATING_ON:
            control_external_power(false);
            if (new_state == RGBLED_POWER_OFF) {
                pm_state.status.total_power_off_time_ms -= k_uptime_get();
            }
            break;
        case RGBLED_POWER_INITIALIZING:
        case RGBLED_POWER_ERROR:
            /* No power control during init/error */
            break;
    }
    
    /* Update statistics */
    if (old_state == RGBLED_POWER_OFF && new_state != RGBLED_POWER_OFF) {
        pm_state.status.total_power_off_time_ms += k_uptime_get();
    }
    
    if (pm_state.config.debug_enabled) {
        LOG_INF("Power state: %d -> %d (trigger: %d)", old_state, new_state, trigger);
    }
    
    /* Notify callbacks */
    notify_callbacks(old_state, new_state, trigger);
    
    /* Schedule next evaluation */
    k_work_schedule(&pm_state.evaluation_work, K_MSEC(RGBLED_POWER_EVAL_INTERVAL_MS));
}

/* Power decision logic */
static bool should_power_off(void) {
    if (!pm_state.config.enabled) {
        return false;
    }
    
    bool battery_trigger = false;
    bool idle_trigger = false;
    
    /* Check battery level */
    if (pm_state.config.battery_detection_enabled) {
        uint8_t battery_level = pm_state.status.battery_level;
        if (battery_level != 255 && battery_level < pm_state.config.battery_threshold) {
            battery_trigger = true;
        }
    }
    
    /* Check idle timeout */
    if (pm_state.config.activity_detection_enabled) {
        int64_t idle_time = k_uptime_get() - pm_state.last_activity_time;
        pm_state.status.idle_time_ms = (uint32_t)idle_time;
        if (idle_time >= pm_state.config.idle_timeout_ms) {
            idle_trigger = true;
        }
    }
    
    return battery_trigger || idle_trigger;
}

static bool should_power_on(void) {
    if (!pm_state.config.enabled) {
        return true;
    }
    
    bool battery_ok = true;
    bool activity_detected = false;
    
    /* Check battery level with hysteresis */
    if (pm_state.config.battery_detection_enabled) {
        uint8_t battery_level = pm_state.status.battery_level;
        uint8_t threshold = pm_state.config.battery_threshold + pm_state.config.battery_hysteresis;
        if (battery_level != 255 && battery_level < threshold) {
            battery_ok = false;
        }
    }
    
    /* Check for recent activity */
    if (pm_state.config.activity_detection_enabled) {
        int64_t idle_time = k_uptime_get() - pm_state.last_activity_time;
        if (idle_time < RGBLED_POWER_EVAL_INTERVAL_MS) {
            activity_detected = true;
        }
    }
    
    return battery_ok && (activity_detected || !pm_state.config.activity_detection_enabled);
}

/* Work handlers */
static void power_evaluation_work_handler(struct k_work *work) {
    if (!pm_state.initialized || pm_state.override_active) {
        return;
    }
    
    /* Update battery level */
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    pm_state.status.battery_level = zmk_battery_state_of_charge();
#else
    pm_state.status.battery_level = 255; /* Unknown */
#endif
    
    /* State machine transitions */
    switch (pm_state.current_state) {
        case RGBLED_POWER_ON:
            if (should_power_off()) {
                set_power_state(RGBLED_POWER_EVALUATING_OFF, 
                               pm_state.status.battery_level < pm_state.config.battery_threshold ? 
                               RGBLED_TRIGGER_BATTERY : RGBLED_TRIGGER_IDLE);
            }
            break;
            
        case RGBLED_POWER_EVALUATING_OFF:
            if (should_power_off()) {
                set_power_state(RGBLED_POWER_OFF, pm_state.status.last_trigger);
            } else {
                set_power_state(RGBLED_POWER_ON, RGBLED_TRIGGER_ACTIVITY);
            }
            break;
            
        case RGBLED_POWER_OFF:
            if (should_power_on()) {
                set_power_state(RGBLED_POWER_EVALUATING_ON, RGBLED_TRIGGER_ACTIVITY);
            }
            break;
            
        case RGBLED_POWER_EVALUATING_ON:
            if (should_power_on()) {
                set_power_state(RGBLED_POWER_ON, RGBLED_TRIGGER_ACTIVITY);
            } else {
                set_power_state(RGBLED_POWER_OFF, RGBLED_TRIGGER_BATTERY);
            }
            break;
            
        default:
            break;
    }
    
    /* Calculate estimated savings */
    int64_t total_uptime = k_uptime_get();
    if (total_uptime > 0) {
        pm_state.status.estimated_savings_percent = 
            (uint8_t)((pm_state.status.total_power_off_time_ms * 100) / total_uptime);
    }
}

static void startup_delay_work_handler(struct k_work *work) {
    LOG_INF("Power management startup delay complete");
    set_power_state(RGBLED_POWER_ON, RGBLED_TRIGGER_INIT);
}

/* Callback management */
static void notify_callbacks(enum rgbled_power_state old_state, enum rgbled_power_state new_state, enum rgbled_power_trigger trigger) {
    for (int i = 0; i < pm_state.callback_count; i++) {
        if (pm_state.callbacks[i]) {
            pm_state.callbacks[i](old_state, new_state, trigger, pm_state.callback_user_data[i]);
        }
    }
}

/* Public API implementation */
int rgbled_power_mgmt_init(void) {
    if (pm_state.initialized) {
        return -EALREADY;
    }
    
    init_default_config();
    
    /* Check external power availability */
#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
    pm_state.ext_power_dev = device_get_binding("EXT_POWER");
    pm_state.external_power_available = (pm_state.ext_power_dev != NULL);
    pm_state.status.ext_power_available = pm_state.external_power_available;
    
    if (pm_state.external_power_available) {
        LOG_INF("External power device found and bound successfully");
    } else {
        LOG_WRN("External power device not available - power management will continue without hardware power switching");
    }
#else
    pm_state.external_power_available = false;
    pm_state.status.ext_power_available = false;
    pm_state.ext_power_dev = NULL;
    LOG_INF("External power support not enabled in configuration");
#endif
    
    /* Initialize work items */
    k_work_init_delayable(&pm_state.evaluation_work, power_evaluation_work_handler);
    k_work_init_delayable(&pm_state.startup_work, startup_delay_work_handler);
    
    /* Initialize timing */
    pm_state.startup_time = k_uptime_get();
    pm_state.last_activity_time = pm_state.startup_time;
    pm_state.last_evaluation_time = pm_state.startup_time;
    
    /* Initialize status */
    pm_state.status.state = RGBLED_POWER_INITIALIZING;
    pm_state.status.last_trigger = RGBLED_TRIGGER_INIT;
    pm_state.status.battery_level = 255;
    pm_state.status.idle_time_ms = 0;
    pm_state.status.total_power_off_time_ms = 0;
    pm_state.status.state_change_count = 0;
    pm_state.status.last_state_change_time = pm_state.startup_time;
    pm_state.status.estimated_savings_percent = 0;
    
    pm_state.initialized = true;
    
    /* Schedule startup delay or immediate activation */
    if (pm_state.config.startup_delay_ms > 0) {
        k_work_schedule(&pm_state.startup_work, K_MSEC(pm_state.config.startup_delay_ms));
    } else {
        set_power_state(RGBLED_POWER_ON, RGBLED_TRIGGER_INIT);
    }
    
    LOG_INF("Power management initialized (ext_power: %s)", 
            pm_state.external_power_available ? "available" : "unavailable");
    
    return 0;
}

int rgbled_power_mgmt_enable(bool enable) {
    if (!pm_state.initialized) {
        return -ENOTSUP;
    }
    
    if (pm_state.current_state == RGBLED_POWER_EVALUATING_OFF || 
        pm_state.current_state == RGBLED_POWER_EVALUATING_ON) {
        return -EBUSY;
    }
    
    pm_state.config.enabled = enable;
    
    if (enable) {
        /* Force immediate evaluation */
        k_work_schedule(&pm_state.evaluation_work, K_NO_WAIT);
    } else {
        /* Force power on when disabled */
        set_power_state(RGBLED_POWER_ON, RGBLED_TRIGGER_MANUAL);
    }
    
    return 0;
}

enum rgbled_power_state rgbled_power_mgmt_get_state(void) {
    return pm_state.current_state;
}

bool rgbled_power_mgmt_is_enabled(void) {
    return pm_state.initialized && pm_state.config.enabled;
}

void rgbled_power_mgmt_notify_activity(void) {
    pm_state.last_activity_time = k_uptime_get();
    
    if (pm_state.current_state == RGBLED_POWER_OFF || 
        pm_state.current_state == RGBLED_POWER_EVALUATING_ON) {
        k_work_schedule(&pm_state.evaluation_work, K_NO_WAIT);
    }
}

bool rgbled_power_mgmt_should_power_leds(void) {
    if (!pm_state.initialized || !pm_state.config.enabled) {
        return true;
    }
    
    return pm_state.current_state == RGBLED_POWER_ON || 
           pm_state.current_state == RGBLED_POWER_EVALUATING_OFF;
}

int rgbled_power_mgmt_get_status(struct rgbled_power_status *status) {
    if (!status) {
        return -EINVAL;
    }
    
    if (!pm_state.initialized) {
        return -ENOTSUP;
    }
    
    *status = pm_state.status;
    return 0;
}

int rgbled_power_mgmt_register_callback(rgbled_power_state_callback_t callback, void *user_data) {
    if (!callback) {
        return -EINVAL;
    }
    
    if (pm_state.callback_count >= RGBLED_POWER_MAX_CALLBACKS) {
        return -ENOMEM;
    }
    
    pm_state.callbacks[pm_state.callback_count] = callback;
    pm_state.callback_user_data[pm_state.callback_count] = user_data;
    pm_state.callback_count++;
    
    return 0;
}

/* Event handlers */
static int power_mgmt_battery_listener(const zmk_event_t *eh) {
    if (!pm_state.initialized) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    k_work_schedule(&pm_state.evaluation_work, K_MSEC(100));
    return ZMK_EV_EVENT_BUBBLE;
}

static int power_mgmt_activity_listener(const zmk_event_t *eh) {
    rgbled_power_mgmt_notify_activity();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(power_mgmt_battery, power_mgmt_battery_listener);
ZMK_SUBSCRIPTION(power_mgmt_battery, zmk_battery_state_changed);

ZMK_LISTENER(power_mgmt_activity, power_mgmt_activity_listener);
ZMK_SUBSCRIPTION(power_mgmt_activity, zmk_activity_state_changed);

#endif /* CONFIG_RGBLED_WIDGET_POWER_MGMT */