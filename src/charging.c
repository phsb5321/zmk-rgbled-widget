/*
 * USB charging indicator. While the keyboard is USB-powered, hold a steady
 * "charging" color on the battery LED; on unplug, clear it so the normal
 * battery indication resumes. The peripheral half sets ZMK_USB=n, so this is
 * central-relevant (the depends-on guard compiles it out where ZMK_USB is off).
 * Default-off.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int rgbled_charging_cb(const zmk_event_t *eh) {
    if (as_zmk_usb_conn_state_changed(eh) == NULL) {
        return 0;
    }
    if (zmk_usb_is_powered()) {
        LOG_INF("rgbled: USB powered -> charging color");
        ws2812_set_status_led(STATUS_BATTERY, CONFIG_RGBLED_WIDGET_CHARGING_COLOR, 0, true);
    } else {
        LOG_INF("rgbled: USB unpowered -> clear charging");
        ws2812_clear_status_led(STATUS_BATTERY);
    }
    return 0;
}

ZMK_LISTENER(rgbled_charging, rgbled_charging_cb);
ZMK_SUBSCRIPTION(rgbled_charging, zmk_usb_conn_state_changed);
