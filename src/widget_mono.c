/**
 * @file widget_mono.c
 * @brief Simple mono-color LED widget implementation for ZMK
 *
 * This implementation provides simple on/off LED control using individual GPIO pins.
 * Supports battery and connectivity status with priority-based sharing.
 * GPIOs can be shared between different status types using different blink patterns.
 *
 * Priority: CONNECT > BAT > LAYER
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_MONO)

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/central.h>
#endif

#include <zmk_rgbled_widget/widget.h>

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT)
#include <zmk_rgbled_widget/power_mgmt.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Internal priority levels for LED sharing (lower number = higher priority)
// Note: Using header file enums mono_led_priority and mono_led_status
enum mono_internal_priority {
    MONO_PRIORITY_CONNECT = MONO_LED_PRIORITY_CONNECT,      // Highest - connectivity status
    MONO_PRIORITY_BATTERY = MONO_LED_PRIORITY_BATTERY,      // Medium - battery status
    MONO_PRIORITY_LAYER = MONO_LED_PRIORITY_LAYER,          // Lowest - layer status
    MONO_PRIORITY_IDLE = 3                                  // No status
};

// Internal status types for mono LEDs
enum mono_status_type {
    MONO_STATUS_NONE = 0,
    MONO_STATUS_BATTERY = MONO_LED_STATUS_BATTERY + 1,
    MONO_STATUS_CONNECTIVITY = MONO_LED_STATUS_CONNECTIVITY + 1,
    MONO_STATUS_LAYER = MONO_LED_STATUS_LAYER + 1
};

// LED state structure for mono LEDs
struct mono_led_state {
    enum mono_status_type status_type;
    enum mono_internal_priority priority;
    bool is_on;
    bool is_blinking;
    uint32_t blink_interval_ms;
    uint32_t timeout_end_time;
    uint32_t next_blink_time;
    uint32_t share_end_time;  // When to return shared LED to original owner
};

// GPIO LED device tree definitions
// Users should define these aliases in their device tree overlay:
// led-bat = &gpio_led_0;     // Battery status LED
// led-connect = &gpio_led_1; // Connectivity status LED
// led-layer = &gpio_led_2;   // Layer status LED (optional, can share with above)

#if DT_NODE_EXISTS(DT_ALIAS(led_bat))
#define BATTERY_LED_NODE DT_ALIAS(led_bat)
static const struct gpio_dt_spec battery_led = GPIO_DT_SPEC_GET(BATTERY_LED_NODE, gpios);
static struct mono_led_state battery_led_state = {0};
#define HAS_BATTERY_LED 1
#else
#define HAS_BATTERY_LED 0
#warning "Battery LED alias 'led-bat' not found in device tree"
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led_connect))
#define CONNECT_LED_NODE DT_ALIAS(led_connect)
static const struct gpio_dt_spec connect_led = GPIO_DT_SPEC_GET(CONNECT_LED_NODE, gpios);
static struct mono_led_state connect_led_state = {0};
#define HAS_CONNECT_LED 1
#else
#define HAS_CONNECT_LED 0
#warning "Connectivity LED alias 'led-connect' not found in device tree"
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led_layer))
#define LAYER_LED_NODE DT_ALIAS(led_layer)
static const struct gpio_dt_spec layer_led = GPIO_DT_SPEC_GET(LAYER_LED_NODE, gpios);
static struct mono_led_state layer_led_state = {0};
#define HAS_LAYER_LED 1
#else
#define HAS_LAYER_LED 0
// Layer LED is optional, can share with battery or connectivity LEDs
#endif

// Status indication request structure
struct mono_led_request {
    enum mono_status_type status_type;
    enum mono_internal_priority priority;
    uint32_t blink_interval_ms;
    uint32_t timeout_ms;
    bool persistent;  // If true, LED stays on without blinking
};

// Message queue for LED status updates
K_MSGQ_DEFINE(mono_led_msgq, sizeof(struct mono_led_request), 16, 1);

// Initialization flag
static bool mono_widget_initialized = false;

// Function declarations
static int mono_set_led_state(const struct gpio_dt_spec *led, struct mono_led_state *state, bool on);
static bool mono_can_use_led(struct mono_led_state *state, enum mono_internal_priority new_priority);
static int mono_indicate_battery(void);
static int mono_indicate_connectivity(void);
static int mono_indicate_layer(void);
static void mono_update_leds(void);
static void mono_check_timeouts(void);

/**
 * Set LED physical state (on/off)
 */
static int mono_set_led_state(const struct gpio_dt_spec *led, struct mono_led_state *state, bool on) {
    if (!gpio_is_ready_dt(led)) {
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT)
    // Check if LEDs should be powered based on power management policy
    if (!rgbled_power_mgmt_should_power_leds()) {
        LOG_DBG("Mono LED not set due to power management policy");
        return 0;
    }
    // Notify power management of LED activity
    rgbled_power_mgmt_notify_activity();
#endif

    int ret = gpio_pin_set_dt(led, on ? 1 : 0);
    if (ret == 0) {
        state->is_on = on;
        LOG_DBG("Set LED to %s (status: %d, priority: %d)",
                on ? "ON" : "OFF", state->status_type, state->priority);
    }
    return ret;
}

/**
 * Check if LED can be used by a new status with given priority
 */
static bool mono_can_use_led(struct mono_led_state *state, enum mono_internal_priority new_priority) {
    if (state->status_type == MONO_STATUS_NONE) {
        return true;  // LED is available
    }

    // Higher priority (lower number) can override lower priority
    return new_priority <= state->priority;
}

/**
 * Request LED status change via message queue
 */
static int mono_request_led_status(enum mono_status_type status_type, uint32_t blink_interval_ms,
                                   uint32_t timeout_ms, bool persistent) {
    struct mono_led_request request = {
        .status_type = status_type,
        .blink_interval_ms = blink_interval_ms,
        .timeout_ms = timeout_ms,
        .persistent = persistent
    };

    // Set priority based on status type
    switch (status_type) {
        case MONO_STATUS_CONNECTIVITY:
            request.priority = MONO_PRIORITY_CONNECT;
            break;
        case MONO_STATUS_BATTERY:
            request.priority = MONO_PRIORITY_BATTERY;
            break;
        case MONO_STATUS_LAYER:
            request.priority = MONO_PRIORITY_LAYER;
            break;
        default:
            return -EINVAL;
    }

    return k_msgq_put(&mono_led_msgq, &request, K_NO_WAIT);
}

/**
 * Battery status indication
 */
static int mono_indicate_battery(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && HAS_BATTERY_LED
    uint8_t battery_level = zmk_battery_state_of_charge();

    // Different blink patterns based on battery level
    uint32_t blink_interval = CONFIG_RGBLED_WIDGET_MONO_BATTERY_BLINK_MS;
    bool persistent = false;

    if (battery_level == 0) {
        // Fast blink for unknown battery level
        blink_interval = 200;
        LOG_INF("Battery level unknown, fast blinking");
    } else if (battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        // Very fast blink for critical battery
        blink_interval = 100;
        LOG_INF("Critical battery level %d%%, very fast blinking", battery_level);
    } else if (battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        // Fast blink for low battery
        blink_interval = CONFIG_RGBLED_WIDGET_MONO_BATTERY_BLINK_MS / 2;
        LOG_INF("Low battery level %d%%, fast blinking", battery_level);
    } else if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        // Solid light for high battery
        persistent = true;
        LOG_INF("High battery level %d%%, solid light", battery_level);
    } else {
        // Normal blink for medium battery
        LOG_INF("Medium battery level %d%%, normal blinking", battery_level);
    }

    return mono_request_led_status(MONO_STATUS_BATTERY, blink_interval,
                                   CONFIG_RGBLED_WIDGET_MONO_TIMEOUT_MS, persistent);
#else
    return -ENOTSUP;
#endif
}

/**
 * Connectivity status indication
 */
static int mono_indicate_connectivity(void) {
#if HAS_CONNECT_LED
    uint32_t blink_interval = CONFIG_RGBLED_WIDGET_MONO_CONN_BLINK_MS;
    bool persistent = false;

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    switch (zmk_endpoints_selected().transport) {
    case ZMK_TRANSPORT_USB:
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
        persistent = true;  // Solid light for USB connection
        LOG_INF("USB connected, solid light");
        break;
#endif
    default: // ZMK_TRANSPORT_BLE
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (zmk_ble_active_profile_is_connected()) {
            persistent = true;  // Solid light for connected
            LOG_INF("BLE connected, solid light");
        } else if (zmk_ble_active_profile_is_open()) {
            blink_interval = CONFIG_RGBLED_WIDGET_MONO_CONN_BLINK_MS;  // Medium blink for advertising
            LOG_INF("BLE advertising, medium blinking");
        } else {
            blink_interval = CONFIG_RGBLED_WIDGET_MONO_CONN_BLINK_MS / 2;  // Fast blink for disconnected
            LOG_INF("BLE disconnected, fast blinking");
        }
#endif
        break;
    }
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
    if (zmk_split_bt_peripheral_is_connected()) {
        persistent = true;  // Solid light for connected peripheral
        LOG_INF("Peripheral connected, solid light");
    } else {
        blink_interval = CONFIG_RGBLED_WIDGET_MONO_CONN_BLINK_MS / 2;  // Fast blink for disconnected
        LOG_INF("Peripheral disconnected, fast blinking");
    }
#endif

    return mono_request_led_status(MONO_STATUS_CONNECTIVITY, blink_interval,
                                   CONFIG_RGBLED_WIDGET_MONO_TIMEOUT_MS, persistent);
#else
    return -ENOTSUP;
#endif
}

/**
 * Layer status indication (future implementation)
 */
static int mono_indicate_layer(void) {
    // Placeholder for future layer indication
    // For now, just show layer change with slow blink
    uint8_t layer_index = zmk_keymap_highest_layer_active();

    LOG_INF("Layer changed to %d, slow blinking", layer_index);

    return mono_request_led_status(MONO_STATUS_LAYER,
                                   CONFIG_RGBLED_WIDGET_MONO_LAYER_BLINK_MS,
                                   CONFIG_RGBLED_WIDGET_MONO_SHARE_TIMEOUT_MS,
                                   false);
}

/**
 * Process LED status request and assign to appropriate LED
 */
static int mono_process_led_request(const struct mono_led_request *request) {
    const struct gpio_dt_spec *target_led = NULL;
    struct mono_led_state *target_state = NULL;
    bool can_use_primary = false;

    uint32_t current_time = k_uptime_get_32();

    // Find primary LED for this status type
    switch (request->status_type) {
        case MONO_STATUS_BATTERY:
#if HAS_BATTERY_LED
            target_led = &battery_led;
            target_state = &battery_led_state;
            can_use_primary = mono_can_use_led(target_state, request->priority);
#endif
            break;

        case MONO_STATUS_CONNECTIVITY:
#if HAS_CONNECT_LED
            target_led = &connect_led;
            target_state = &connect_led_state;
            can_use_primary = mono_can_use_led(target_state, request->priority);
#endif
            break;

        case MONO_STATUS_LAYER:
#if HAS_LAYER_LED
            target_led = &layer_led;
            target_state = &layer_led_state;
            can_use_primary = mono_can_use_led(target_state, request->priority);
#endif
            // If no dedicated layer LED or can't use it, try to share other LEDs
            if (!can_use_primary) {
                // Try to share connectivity LED first
#if HAS_CONNECT_LED
                if (mono_can_use_led(&connect_led_state, request->priority)) {
                    target_led = &connect_led;
                    target_state = &connect_led_state;
                    can_use_primary = true;
                }
#endif
                // Then try battery LED
#if HAS_BATTERY_LED
                else if (mono_can_use_led(&battery_led_state, request->priority)) {
                    target_led = &battery_led;
                    target_state = &battery_led_state;
                    can_use_primary = true;
                }
#endif
            }
            break;

        default:
            return -EINVAL;
    }

    if (!target_led || !can_use_primary) {
        LOG_WRN("Cannot assign LED for status %d (priority %d)",
                request->status_type, request->priority);
        return -EBUSY;
    }

    // Update LED state
    target_state->status_type = request->status_type;
    target_state->priority = request->priority;
    target_state->blink_interval_ms = request->blink_interval_ms;
    target_state->is_blinking = !request->persistent;
    target_state->timeout_end_time = current_time + request->timeout_ms;
    target_state->next_blink_time = current_time + request->blink_interval_ms;

    // Set share timeout for shared LEDs
    if (request->status_type == MONO_STATUS_LAYER &&
        (target_led == &connect_led || target_led == &battery_led)) {
        target_state->share_end_time = current_time + CONFIG_RGBLED_WIDGET_MONO_SHARE_TIMEOUT_MS;
    } else {
        target_state->share_end_time = 0;  // Not shared
    }

    // Set initial LED state
    if (request->persistent) {
        mono_set_led_state(target_led, target_state, true);
    } else {
        mono_set_led_state(target_led, target_state, true);  // Start with LED on
    }

    LOG_DBG("Assigned status %d to LED (priority %d, blink: %s)",
            request->status_type, request->priority,
            request->persistent ? "no" : "yes");

    return 0;
}

/**
 * Update LED states (handle blinking)
 */
static void mono_update_leds(void) {
    uint32_t current_time = k_uptime_get_32();

#if HAS_BATTERY_LED
    struct mono_led_state *state = &battery_led_state;
    if (state->status_type != MONO_STATUS_NONE && state->is_blinking) {
        if (current_time >= state->next_blink_time) {
            mono_set_led_state(&battery_led, state, !state->is_on);
            state->next_blink_time = current_time + state->blink_interval_ms;
        }
    }
#endif

#if HAS_CONNECT_LED
    state = &connect_led_state;
    if (state->status_type != MONO_STATUS_NONE && state->is_blinking) {
        if (current_time >= state->next_blink_time) {
            mono_set_led_state(&connect_led, state, !state->is_on);
            state->next_blink_time = current_time + state->blink_interval_ms;
        }
    }
#endif

#if HAS_LAYER_LED
    state = &layer_led_state;
    if (state->status_type != MONO_STATUS_NONE && state->is_blinking) {
        if (current_time >= state->next_blink_time) {
            mono_set_led_state(&layer_led, state, !state->is_on);
            state->next_blink_time = current_time + state->blink_interval_ms;
        }
    }
#endif
}

/**
 * Check for expired timeouts and shared LED returns
 */
static void mono_check_timeouts(void) {
    uint32_t current_time = k_uptime_get_32();

#if HAS_BATTERY_LED
    struct mono_led_state *state = &battery_led_state;
    if (state->status_type != MONO_STATUS_NONE) {
        // Check share timeout (return to original owner)
        if (state->share_end_time > 0 && current_time >= state->share_end_time) {
            LOG_DBG("Battery LED share timeout, clearing status %d", state->status_type);
            mono_set_led_state(&battery_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
        // Check main timeout (turn off LED)
        else if (current_time >= state->timeout_end_time) {
            LOG_DBG("Battery LED timeout, clearing status %d", state->status_type);
            mono_set_led_state(&battery_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
    }
#endif

#if HAS_CONNECT_LED
    state = &connect_led_state;
    if (state->status_type != MONO_STATUS_NONE) {
        if (state->share_end_time > 0 && current_time >= state->share_end_time) {
            LOG_DBG("Connect LED share timeout, clearing status %d", state->status_type);
            mono_set_led_state(&connect_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
        else if (current_time >= state->timeout_end_time) {
            LOG_DBG("Connect LED timeout, clearing status %d", state->status_type);
            mono_set_led_state(&connect_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
    }
#endif

#if HAS_LAYER_LED
    state = &layer_led_state;
    if (state->status_type != MONO_STATUS_NONE) {
        if (state->share_end_time > 0 && current_time >= state->share_end_time) {
            LOG_DBG("Layer LED share timeout, clearing status %d", state->status_type);
            mono_set_led_state(&layer_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
        else if (current_time >= state->timeout_end_time) {
            LOG_DBG("Layer LED timeout, clearing status %d", state->status_type);
            mono_set_led_state(&layer_led, state, false);
            memset(state, 0, sizeof(*state));
            state->priority = MONO_PRIORITY_IDLE;
        }
    }
#endif
}

// Event listeners and handlers

static int mono_battery_listener_cb(const zmk_event_t *eh) {
    if (!mono_widget_initialized) {
        return 0;
    }

    // Indicate battery status on battery level change
    mono_indicate_battery();
    return 0;
}

static int mono_output_listener_cb(const zmk_event_t *eh) {
    if (!mono_widget_initialized) {
        return 0;
    }

    // Indicate connectivity status on connection change
    mono_indicate_connectivity();
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_KEYMAP)
static int mono_layer_listener_cb(const zmk_event_t *eh) {
    if (!mono_widget_initialized) {
        return 0;
    }

    // Indicate layer change (future implementation)
    mono_indicate_layer();
    return 0;
}
#endif

// Event listener registrations
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_LISTENER(mono_battery_listener, mono_battery_listener_cb);
ZMK_SUBSCRIPTION(mono_battery_listener, zmk_battery_state_changed);
#endif

ZMK_LISTENER(mono_output_listener, mono_output_listener_cb);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
ZMK_SUBSCRIPTION(mono_output_listener, zmk_endpoint_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(mono_output_listener, zmk_ble_active_profile_changed);
#endif
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
ZMK_SUBSCRIPTION(mono_output_listener, zmk_split_peripheral_status_changed);
#endif

// Layer listener (optional, for future layer indication)
#if IS_ENABLED(CONFIG_ZMK_KEYMAP)
ZMK_LISTENER(mono_layer_listener, mono_layer_listener_cb);
ZMK_SUBSCRIPTION(mono_layer_listener, zmk_layer_state_changed);
#endif

/**
 * LED processing thread
 */
static void mono_led_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    LOG_INF("Mono LED thread started");

    for (;;) {
        struct mono_led_request request;
        int ret = k_msgq_get(&mono_led_msgq, &request, K_MSEC(100));

        if (ret == 0) {
            // Process new LED request
            mono_process_led_request(&request);
        }

        // Update LED states (handle blinking)
        mono_update_leds();

        // Check for timeouts
        mono_check_timeouts();
    }
}

K_THREAD_DEFINE(mono_led_tid, 1024, mono_led_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

/**
 * Initialize mono LED widget
 */
static int mono_widget_init(void) {
    LOG_INF("Initializing mono LED widget");

    // Initialize GPIO LEDs
#if HAS_BATTERY_LED
    if (!gpio_is_ready_dt(&battery_led)) {
        LOG_ERR("Battery LED GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&battery_led, GPIO_OUTPUT_INACTIVE);
    battery_led_state.priority = MONO_PRIORITY_IDLE;
    LOG_INF("Battery LED initialized");
#endif

#if HAS_CONNECT_LED
    if (!gpio_is_ready_dt(&connect_led)) {
        LOG_ERR("Connectivity LED GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&connect_led, GPIO_OUTPUT_INACTIVE);
    connect_led_state.priority = MONO_PRIORITY_IDLE;
    LOG_INF("Connectivity LED initialized");
#endif

#if HAS_LAYER_LED
    if (!gpio_is_ready_dt(&layer_led)) {
        LOG_ERR("Layer LED GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&layer_led, GPIO_OUTPUT_INACTIVE);
    layer_led_state.priority = MONO_PRIORITY_IDLE;
    LOG_INF("Layer LED initialized");
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_POWER_MGMT)
    // Initialize power management if enabled
    int ret = rgbled_power_mgmt_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize power management: %d", ret);
    } else {
        LOG_INF("Power management initialized for mono LEDs");
    }
#endif

    mono_widget_initialized = true;

    // Show initial status
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    k_sleep(K_MSEC(500));  // Brief delay for system stability
    mono_indicate_battery();
#endif

    mono_indicate_connectivity();

    LOG_INF("Mono LED widget initialized successfully");
    return 0;
}

/**
 * Initialization thread
 */
static void mono_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    mono_widget_init();
}

K_THREAD_DEFINE(mono_init_tid, 512, mono_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);

// Public API functions (compatible with existing widget interface)

void indicate_battery(void) {
    mono_indicate_battery();
}

void indicate_connectivity(void) {
    mono_indicate_connectivity();
}

void indicate_layer(void) {
    mono_indicate_layer();
}

#endif // CONFIG_RGBLED_WIDGET_MONO
