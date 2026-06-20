/*
 * Central-side split peer-lost indicator.
 *
 * zmk_split_peripheral_status_changed only fires on the PERIPHERAL, so on the
 * CENTRAL we register a raw Zephyr bt_conn callback and filter for the split
 * link: on the central, the connection to the peripheral half has
 * info.role == BT_CONN_ROLE_CENTRAL (this device is the BLE-central of that
 * link), whereas the link to the host has role PERIPHERAL. On the peripheral
 * half all links are PERIPHERAL-role, so the callback no-ops there — harmless if
 * the symbol is ever compiled on both halves.
 *
 * On drop  -> a transient blink in the "lost" color on the connectivity LED.
 * On (re)connect -> a transient blink in the "ok" color.
 * Transient (not persistent) so it signals the transition without permanently
 * holding the connectivity slot. Default-off.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/conn.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool is_split_peer_link(struct bt_conn *conn) {
    struct bt_conn_info info;
    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}

static void rgbled_peer_connected(struct bt_conn *conn, uint8_t err) {
    if (err != 0 || !is_split_peer_link(conn)) {
        return;
    }
    LOG_INF("rgbled: split peer connected");
    ws2812_set_status_led(STATUS_CONNECTIVITY, CONFIG_RGBLED_WIDGET_PEER_STATUS_OK_COLOR,
                          CONFIG_RGBLED_WIDGET_PEER_STATUS_BLINK_MS, false);
}

static void rgbled_peer_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (!is_split_peer_link(conn)) {
        return;
    }
    LOG_INF("rgbled: split peer lost (reason 0x%02x)", reason);
    ws2812_set_status_led(STATUS_CONNECTIVITY, CONFIG_RGBLED_WIDGET_PEER_STATUS_LOST_COLOR,
                          CONFIG_RGBLED_WIDGET_PEER_STATUS_BLINK_MS, false);
}

BT_CONN_CB_DEFINE(rgbled_peer_status) = {
    .connected = rgbled_peer_connected,
    .disconnected = rgbled_peer_disconnected,
};
