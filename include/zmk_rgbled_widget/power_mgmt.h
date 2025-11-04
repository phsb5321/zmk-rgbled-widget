#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file power_mgmt.h
 * @brief RGB LED Power Management API
 * 
 * This module provides intelligent power management for RGB LEDs on battery-powered
 * keyboards, offering up to 75% power savings through automatic external power control
 * based on battery levels and user activity patterns.
 */

/**
 * @brief Power management state enumeration
 * 
 * Represents the current state of the power management system.
 */
enum rgbled_power_state {
    /** System initializing, power management not yet active */
    RGBLED_POWER_INITIALIZING = 0,
    
    /** LEDs powered on, normal operation */
    RGBLED_POWER_ON = 1,
    
    /** Evaluating whether to power off based on conditions */
    RGBLED_POWER_EVALUATING_OFF = 2,
    
    /** LEDs powered off to conserve battery */
    RGBLED_POWER_OFF = 3,
    
    /** Evaluating whether to power back on */
    RGBLED_POWER_EVALUATING_ON = 4,
    
    /** Error state - power management disabled */
    RGBLED_POWER_ERROR = 5
};

/**
 * @brief Power management trigger reasons
 * 
 * Indicates what triggered a power state change.
 */
enum rgbled_power_trigger {
    /** Power change due to battery level threshold */
    RGBLED_TRIGGER_BATTERY = 0,
    
    /** Power change due to idle timeout */
    RGBLED_TRIGGER_IDLE = 1,
    
    /** Power change due to keyboard activity */
    RGBLED_TRIGGER_ACTIVITY = 2,
    
    /** Power change due to manual override */
    RGBLED_TRIGGER_MANUAL = 3,
    
    /** Power change due to external power availability */
    RGBLED_TRIGGER_EXT_POWER = 4,
    
    /** Power change during initialization */
    RGBLED_TRIGGER_INIT = 5
};

/**
 * @brief Power management configuration structure
 * 
 * Runtime-modifiable configuration for power management behavior.
 */
struct rgbled_power_config {
    /** Master enable for power management */
    bool enabled;
    
    /** Battery percentage threshold for power cutoff (5-95) */
    uint8_t battery_threshold;
    
    /** Hysteresis percentage to prevent oscillation (1-20) */
    uint8_t battery_hysteresis;
    
    /** Idle timeout in milliseconds before power off */
    uint32_t idle_timeout_ms;
    
    /** Startup delay before power management becomes active */
    uint32_t startup_delay_ms;
    
    /** Enable activity-based power management */
    bool activity_detection_enabled;
    
    /** Enable battery-level based power management */
    bool battery_detection_enabled;
    
    /** Enable debug logging */
    bool debug_enabled;
};

/**
 * @brief Power management status structure
 * 
 * Current status and statistics of the power management system.
 */
struct rgbled_power_status {
    /** Current power state */
    enum rgbled_power_state state;
    
    /** Last trigger that caused state change */
    enum rgbled_power_trigger last_trigger;
    
    /** Current battery level percentage (0-100, 255 if unknown) */
    uint8_t battery_level;
    
    /** Time since last activity in milliseconds */
    uint32_t idle_time_ms;
    
    /** External power availability */
    bool ext_power_available;
    
    /** Total time powered off (milliseconds) */
    uint64_t total_power_off_time_ms;
    
    /** Number of power state changes */
    uint32_t state_change_count;
    
    /** Timestamp of last state change */
    int64_t last_state_change_time;
    
    /** Estimated power savings percentage (0-100) */
    uint8_t estimated_savings_percent;
};

/**
 * @brief Power state change callback function type
 * 
 * @param old_state Previous power state
 * @param new_state New power state
 * @param trigger What triggered the state change
 * @param user_data User-provided data
 */
typedef void (*rgbled_power_state_callback_t)(enum rgbled_power_state old_state,
                                             enum rgbled_power_state new_state,
                                             enum rgbled_power_trigger trigger,
                                             void *user_data);

/* Public API Functions */

/**
 * @brief Initialize the power management subsystem
 * 
 * This function must be called before using any other power management functions.
 * It initializes the state machine, registers event handlers, and sets up
 * external power control interfaces.
 * 
 * @retval 0 Success
 * @retval -ENODEV External power device not available
 * @retval -EALREADY Already initialized
 * @retval -ENOMEM Insufficient memory
 */
int rgbled_power_mgmt_init(void);

/**
 * @brief Enable or disable power management
 * 
 * @param enable True to enable, false to disable power management
 * 
 * @retval 0 Success
 * @retval -ENOTSUP Power management not available
 * @retval -EBUSY Cannot change state while evaluating
 */
int rgbled_power_mgmt_enable(bool enable);

/**
 * @brief Set battery threshold for power cutoff
 * 
 * @param threshold Battery percentage (5-95) below which LEDs are powered off
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid threshold value
 */
int rgbled_power_mgmt_set_threshold(uint8_t threshold);

/**
 * @brief Set battery hysteresis percentage
 * 
 * @param hysteresis Percentage (1-20) added to threshold for power-on
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid hysteresis value
 */
int rgbled_power_mgmt_set_hysteresis(uint8_t hysteresis);

/**
 * @brief Set idle timeout for activity-based power management
 * 
 * @param timeout_ms Timeout in milliseconds (30000-3600000)
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid timeout value
 */
int rgbled_power_mgmt_set_idle_timeout(uint32_t timeout_ms);

/**
 * @brief Get current power state
 * 
 * @return Current power state
 */
enum rgbled_power_state rgbled_power_mgmt_get_state(void);

/**
 * @brief Check if power management is enabled
 * 
 * @return True if enabled, false otherwise
 */
bool rgbled_power_mgmt_is_enabled(void);

/**
 * @brief Force immediate power state evaluation
 * 
 * Forces the power management system to immediately evaluate current conditions
 * and update power state if necessary.
 * 
 * @retval 0 Success
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_force_update(void);

/**
 * @brief Get comprehensive power management status
 * 
 * @param status Pointer to status structure to fill
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid status pointer
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_get_status(struct rgbled_power_status *status);

/**
 * @brief Get current configuration
 * 
 * @param config Pointer to configuration structure to fill
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid config pointer
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_get_config(struct rgbled_power_config *config);

/**
 * @brief Update configuration settings
 * 
 * @param config Pointer to new configuration
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid config pointer or values
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_set_config(const struct rgbled_power_config *config);

/**
 * @brief Register power state change callback
 * 
 * @param callback Callback function to register
 * @param user_data User data to pass to callback
 * 
 * @retval 0 Success
 * @retval -EINVAL Invalid callback pointer
 * @retval -ENOMEM No callback slots available
 */
int rgbled_power_mgmt_register_callback(rgbled_power_state_callback_t callback,
                                      void *user_data);

/**
 * @brief Unregister power state change callback
 * 
 * @param callback Callback function to unregister
 * 
 * @retval 0 Success
 * @retval -ENOENT Callback not found
 */
int rgbled_power_mgmt_unregister_callback(rgbled_power_state_callback_t callback);

/**
 * @brief Manually override power state
 * 
 * Temporarily overrides automatic power management and forces LEDs to
 * a specific power state. Override is cleared on next automatic evaluation.
 * 
 * @param power_on True to force power on, false to force power off
 * @param duration_ms Duration of override in milliseconds (0 = permanent until next evaluation)
 * 
 * @retval 0 Success
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_override(bool power_on, uint32_t duration_ms);

/**
 * @brief Clear any active power state override
 * 
 * @retval 0 Success
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_clear_override(void);

/**
 * @brief Reset power management statistics
 * 
 * Clears accumulated statistics like total power-off time and state change counts.
 * 
 * @retval 0 Success
 * @retval -ENOTSUP Power management not initialized
 */
int rgbled_power_mgmt_reset_stats(void);

/* Internal API for integration with widget.c */

/**
 * @brief Notify power management of LED activity
 * 
 * This function should be called whenever LED activity occurs to reset
 * the idle timer for activity-based power management.
 * 
 * @note This is an internal API used by the widget system
 */
void rgbled_power_mgmt_notify_activity(void);

/**
 * @brief Check if LEDs should be powered based on current policy
 * 
 * @return True if LEDs should be powered on, false otherwise
 * 
 * @note This is an internal API used by the widget system
 */
bool rgbled_power_mgmt_should_power_leds(void);

/* Configuration constants */

/** Maximum number of registered callbacks */
#define RGBLED_POWER_MAX_CALLBACKS 4

/** Minimum battery threshold percentage */
#define RGBLED_POWER_MIN_BATTERY_THRESHOLD 5

/** Maximum battery threshold percentage */
#define RGBLED_POWER_MAX_BATTERY_THRESHOLD 95

/** Minimum hysteresis percentage */
#define RGBLED_POWER_MIN_HYSTERESIS 1

/** Maximum hysteresis percentage */
#define RGBLED_POWER_MAX_HYSTERESIS 20

/** Minimum idle timeout (30 seconds) */
#define RGBLED_POWER_MIN_IDLE_TIMEOUT_MS 30000

/** Maximum idle timeout (1 hour) */
#define RGBLED_POWER_MAX_IDLE_TIMEOUT_MS 3600000

/** Power management evaluation interval (1 second) */
#define RGBLED_POWER_EVAL_INTERVAL_MS 1000

#ifdef __cplusplus
}
#endif