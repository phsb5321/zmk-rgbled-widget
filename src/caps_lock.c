/*
 * Caps-lock indicator for the WS2812 status LEDs.
 *
 * Lights the STATUS_CUSTOM LED in CONFIG_RGBLED_WIDGET_CAPS_LOCK_COLOR while
 * the host reports caps-lock ON (HID keyboard-LED output report), and clears
 * it when caps-lock turns off. Net-new feature (absent from caksoylar /
 * hitsmaxft). Default-off; the consumer must also enable
 * CONFIG_ZMK_HID_INDICATORS so the host LED state reaches the keyboard. On a
 * split, HID indicators are reported on the central, so this binds there.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int rgbled_caps_lock_cb(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    bool caps_on = (ev->indicators & HID_INDICATOR_CAPS_LOCK) != 0;
    LOG_DBG("rgbled caps-lock %s", caps_on ? "on" : "off");

    if (caps_on) {
        ws2812_set_status_led(STATUS_CUSTOM, CONFIG_RGBLED_WIDGET_CAPS_LOCK_COLOR, 0, true);
    } else {
        ws2812_clear_status_led(STATUS_CUSTOM);
    }

    return 0;
}

ZMK_LISTENER(rgbled_caps_lock, rgbled_caps_lock_cb);
ZMK_SUBSCRIPTION(rgbled_caps_lock, zmk_hid_indicators_changed);
