#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * User configuration
 * ============================================================ */

#define WIFI_SSID       "Iphone 67"
#define WIFI_PSK        "sunasuna"

#define JETSON_IP       "192.168.1.54"
#define JETSON_PORT     5000

#define RESULT_PATH     "/latest_result.json"
#define IMAGE_URL       "http://" JETSON_IP ":5000/latest_image.jpg"

#define CAMERA_COMMAND_PATH "/command/take_photo"

#define ALARM_NODE_NAME "UV_ALARM_NODE"

#define DASHBOARD_BAUD  115200

#define JETSON_POLL_PERIOD_MS  3000
#define UV_READ_PERIOD_MS      10000
#define HEARTBEAT_PERIOD_MS    5000

#define MAX_HTTP_BODY_BYTES    2048
#define MAX_HTTP_RX_BYTES      3072
#define MAX_SERIAL_LINE_BYTES  256

/* ============================================================
 * NUS UUIDs
 * ============================================================ */

#define BT_UUID_NUS_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

#define BT_UUID_NUS_RX_VAL \
    BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

#define BT_UUID_NUS_TX_VAL \
    BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

static struct bt_uuid_128 nus_service_uuid =
    BT_UUID_INIT_128(BT_UUID_NUS_SERVICE_VAL);

static struct bt_uuid_128 nus_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_NUS_RX_VAL);

static struct bt_uuid_128 nus_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_NUS_TX_VAL);

/* ============================================================
 * Global state
 * ============================================================ */

static const struct device *dashboard_uart;

static K_SEM_DEFINE(wifi_connected_sem, 0, 1);
static K_SEM_DEFINE(ipv4_addr_sem, 0, 1);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static bool wifi_callbacks_registered;
static bool wifi_associated;

static K_MUTEX_DEFINE(serial_mutex);
static K_MUTEX_DEFINE(http_mutex);

static bool wifi_ready;
static bool alarm_state;
static int32_t last_forwarded_frame_id = -1;

static uint8_t http_rx_buf[MAX_HTTP_RX_BYTES];
static char http_body[MAX_HTTP_BODY_BYTES];

/* BLE state */
static struct bt_conn *alarm_conn;

static uint16_t nus_service_start_handle;
static uint16_t nus_service_end_handle;
static uint16_t nus_rx_handle;
static uint16_t nus_tx_handle;
static uint16_t nus_tx_ccc_handle;

static uint8_t ccc_discover_retries;
#define MAX_CCC_RETRIES 4

static bool nus_ready;

static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;

/* Threads */
K_THREAD_STACK_DEFINE(wifi_stack, 4096);
static struct k_thread wifi_thread_data;

K_THREAD_STACK_DEFINE(serial_rx_stack, 2048);
static struct k_thread serial_rx_thread_data;

K_THREAD_STACK_DEFINE(jetson_poll_stack, 4096);
static struct k_thread jetson_poll_thread_data;

K_THREAD_STACK_DEFINE(uv_poll_stack, 2048);
static struct k_thread uv_poll_thread_data;

K_THREAD_STACK_DEFINE(heartbeat_stack, 2048);
static struct k_thread heartbeat_thread_data;

/* ============================================================
 * PPE result model
 * ============================================================ */

struct ppe_result {
    int32_t frame_id;
    char node_id[32];
    bool ppe_detected;
    char missing_items_json[160];
    char confidence[24];
    char action[24];
};

/* ============================================================
 * Serial dashboard helpers
 * ============================================================ */

static void serial_send_bytes_unlocked(const uint8_t *data, size_t len)
{
    if (!dashboard_uart || !device_is_ready(dashboard_uart)) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(dashboard_uart, data[i]);
    }
}

static void serial_send_bytes(const uint8_t *data, size_t len)
{
    k_mutex_lock(&serial_mutex, K_FOREVER);
    serial_send_bytes_unlocked(data, len);
    k_mutex_unlock(&serial_mutex);
}

static void serial_send_str(const char *s)
{
    serial_send_bytes((const uint8_t *)s, strlen(s));
}

static void dashboard_send_status(const char *status)
{
    char line[192];

    snprintf(line, sizeof(line),
             "{\"type\":\"status\","
             "\"node_id\":\"base_node\","
             "\"status\":\"%s\","
             "\"wifi\":%s,"
             "\"nus\":%s,"
             "\"alarm\":%s}\n",
             status,
             wifi_ready ? "true" : "false",
             nus_ready ? "true" : "false",
             alarm_state ? "true" : "false");

    serial_send_str(line);
}

static void dashboard_send_alert(const char *message)
{
    char line[256];

    snprintf(line, sizeof(line),
             "{\"type\":\"alert\","
             "\"node_id\":\"base_node\","
             "\"message\":\"%s\"}\n",
             message);

    serial_send_str(line);
}

static int dashboard_uart_init(void)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_console))
    dashboard_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#else
    dashboard_uart = NULL;
    return -ENODEV;
#endif

    if (!device_is_ready(dashboard_uart)) {
        return -ENODEV;
    }

    struct uart_config cfg = {
        .baudrate = DASHBOARD_BAUD,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };

    (void)uart_configure(dashboard_uart, &cfg);

    return 0;
}

/* ============================================================
 * Minimal JSON field extraction
 * ============================================================ */

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    return strstr(json, pattern);
}

static int extract_string_field(const char *json,
                                const char *key,
                                char *out,
                                size_t out_len)
{
    const char *p = find_json_key(json, key);

    if (!p) {
        return -ENOENT;
    }

    p = strchr(p, ':');

    if (!p) {
        return -EINVAL;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '"') {
        return -EINVAL;
    }

    p++;

    const char *end = strchr(p, '"');

    if (!end) {
        return -EINVAL;
    }

    size_t len = MIN((size_t)(end - p), out_len - 1);
    memcpy(out, p, len);
    out[len] = '\0';

    return 0;
}

static int extract_int_field(const char *json, const char *key, int32_t *out)
{
    const char *p = find_json_key(json, key);

    if (!p) {
        return -ENOENT;
    }

    p = strchr(p, ':');

    if (!p) {
        return -EINVAL;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    *out = (int32_t)strtol(p, NULL, 10);
    return 0;
}

static int extract_bool_field(const char *json, const char *key, bool *out)
{
    const char *p = find_json_key(json, key);

    if (!p) {
        return -ENOENT;
    }

    p = strchr(p, ':');

    if (!p) {
        return -EINVAL;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return 0;
    }

    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return 0;
    }

    return -EINVAL;
}

static int extract_number_as_string(const char *json,
                                    const char *key,
                                    char *out,
                                    size_t out_len)
{
    const char *p = find_json_key(json, key);

    if (!p) {
        return -ENOENT;
    }

    p = strchr(p, ':');

    if (!p) {
        return -EINVAL;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    const char *start = p;

    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
        p++;
    }

    if (p == start) {
        return -EINVAL;
    }

    size_t len = MIN((size_t)(p - start), out_len - 1);
    memcpy(out, start, len);
    out[len] = '\0';

    return 0;
}

static int extract_array_raw(const char *json,
                             const char *key,
                             char *out,
                             size_t out_len)
{
    const char *p = find_json_key(json, key);

    if (!p) {
        return -ENOENT;
    }

    p = strchr(p, ':');

    if (!p) {
        return -EINVAL;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '[') {
        return -EINVAL;
    }

    const char *start = p;
    const char *end = strchr(p, ']');

    if (!end) {
        return -EINVAL;
    }

    end++;

    size_t len = MIN((size_t)(end - start), out_len - 1);
    memcpy(out, start, len);
    out[len] = '\0';

    return 0;
}

static void parse_ppe_result(char *json, struct ppe_result *out)
{
    memset(out, 0, sizeof(*out));

    out->frame_id = -1;
    strcpy(out->node_id, "camera_01");
    strcpy(out->missing_items_json, "[]");
    strcpy(out->confidence, "0.0");
    strcpy(out->action, "none");
    out->ppe_detected = true;

    (void)extract_int_field(json, "frame_id", &out->frame_id);
    (void)extract_string_field(json, "node_id", out->node_id, sizeof(out->node_id));
    (void)extract_bool_field(json, "ppe_detected", &out->ppe_detected);
    (void)extract_array_raw(json, "missing_items",
                            out->missing_items_json,
                            sizeof(out->missing_items_json));
    (void)extract_number_as_string(json, "confidence",
                                   out->confidence,
                                   sizeof(out->confidence));
    (void)extract_string_field(json, "action", out->action, sizeof(out->action));
}

static bool missing_items_nonempty(const char *array_json)
{
    if (!array_json) {
        return false;
    }

    return strstr(array_json, "\"") != NULL;
}

static bool ppe_result_requires_alarm(const struct ppe_result *r)
{
    if (strcmp(r->action, "alert") == 0) {
        return true;
    }

    if (!r->ppe_detected) {
        return true;
    }

    if (missing_items_nonempty(r->missing_items_json)) {
        return true;
    }

    return false;
}

/* ============================================================
 * Dashboard forwarding
 * ============================================================ */

static void dashboard_send_ai_result(const struct ppe_result *r)
{
    char line[512];

    snprintf(line, sizeof(line),
             "{\"type\":\"ai_result\","
             "\"frame_id\":%ld,"
             "\"node_id\":\"%s\","
             "\"ppe_detected\":%s,"
             "\"missing_items\":%s,"
             "\"confidence\":%s,"
             "\"action\":\"%s\"}\n",
             (long)r->frame_id,
             r->node_id,
             r->ppe_detected ? "true" : "false",
             r->missing_items_json,
             r->confidence,
             r->action);

    serial_send_str(line);
}

static void dashboard_send_processed_image_url(const struct ppe_result *r)
{
    char line[384];

    snprintf(line, sizeof(line),
             "{\"type\":\"processed_image\","
             "\"frame_id\":%ld,"
             "\"node_id\":\"%s\","
             "\"image_url\":\"%s\"}\n",
             (long)r->frame_id,
             r->node_id,
             IMAGE_URL);

    serial_send_str(line);
}

/* ============================================================
 * HTTP client helpers
 * ============================================================ */

static int find_header_end(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)i + 4;
        }
    }

    return -1;
}

static bool http_status_is_2xx(const uint8_t *buf, size_t len)
{
    if (len < 12) {
        return false;
    }

    return memcmp(buf, "HTTP/1.1 2", 10) == 0 ||
           memcmp(buf, "HTTP/1.0 2", 10) == 0;
}

static int http_open_socket(void)
{
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (sock < 0) {
        return -errno;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(JETSON_PORT);

    if (zsock_inet_pton(AF_INET, JETSON_IP, &addr.sin_addr) != 1) {
        zsock_close(sock);
        return -EINVAL;
    }

    if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = -errno;
        zsock_close(sock);
        return err;
    }

    return sock;
}

static int http_get_body(const char *path,
                         char *body,
                         size_t body_cap,
                         size_t *body_len_out)
{
    int ret = 0;
    int sock = -1;

    *body_len_out = 0;

    k_mutex_lock(&http_mutex, K_FOREVER);

    memset(http_rx_buf, 0, sizeof(http_rx_buf));

    sock = http_open_socket();

    if (sock < 0) {
        ret = sock;
        goto out;
    }

    char request[256];

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n"
             "\r\n",
             path,
             JETSON_IP,
             JETSON_PORT);

    if (zsock_send(sock, request, strlen(request), 0) < 0) {
        ret = -errno;
        goto out;
    }

    size_t total = 0;

    while (total < sizeof(http_rx_buf)) {
        int received = zsock_recv(sock,
                                  http_rx_buf + total,
                                  sizeof(http_rx_buf) - total,
                                  0);

        if (received < 0) {
            ret = -errno;
            goto out;
        }

        if (received == 0) {
            break;
        }

        total += received;
    }

    if (total == sizeof(http_rx_buf)) {
        ret = -ENOMEM;
        goto out;
    }

    if (!http_status_is_2xx(http_rx_buf, total)) {
        ret = -EIO;
        goto out;
    }

    int body_start = find_header_end(http_rx_buf, total);

    if (body_start < 0) {
        ret = -EINVAL;
        goto out;
    }

    size_t body_len = total - (size_t)body_start;

    if (body_len >= body_cap) {
        ret = -ENOMEM;
        goto out;
    }

    memcpy(body, http_rx_buf + body_start, body_len);
    body[body_len] = '\0';
    *body_len_out = body_len;

out:
    if (sock >= 0) {
        zsock_close(sock);
    }

    k_mutex_unlock(&http_mutex);
    return ret;
}

static int http_post_json(const char *path, const char *json_body)
{
    int ret = 0;
    int sock = -1;

    k_mutex_lock(&http_mutex, K_FOREVER);

    sock = http_open_socket();

    if (sock < 0) {
        ret = sock;
        goto out;
    }

    char request[512];
    size_t body_len = strlen(json_body);

    snprintf(request, sizeof(request),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             path,
             JETSON_IP,
             JETSON_PORT,
             (unsigned)body_len,
             json_body);

    if (zsock_send(sock, request, strlen(request), 0) < 0) {
        ret = -errno;
        goto out;
    }

    /*
     * Read and discard response so the server can close cleanly.
     */
    (void)zsock_recv(sock, http_rx_buf, sizeof(http_rx_buf), 0);

out:
    if (sock >= 0) {
        zsock_close(sock);
    }

    k_mutex_unlock(&http_mutex);
    return ret;
}

/* ============================================================
 * Wi-Fi
 * ============================================================ */

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{
    ARG_UNUSED(iface);

    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;

        if (status && status->status == 0) {
            wifi_associated = true;
            dashboard_send_status("wifi_associated");
            k_sem_give(&wifi_connected_sem);
        } else {
            wifi_associated = false;
            wifi_ready = false;

            char msg[96];
            snprintf(msg, sizeof(msg),
                     "wifi_connect_failed_code_%d",
                     status ? status->status : -999);
            dashboard_send_status(msg);

            k_sem_give(&wifi_connected_sem);
        }
    }

    if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        wifi_associated = false;
        wifi_ready = false;
        dashboard_send_status("wifi_disconnected");
    }
}



static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        wifi_ready = true;
        dashboard_send_status("wifi_ipv4_ready");
        k_sem_give(&ipv4_addr_sem);
    }
}

static void wifi_events_init_once(void)
{
    if (wifi_callbacks_registered) {
        return;
    }

    net_mgmt_init_event_callback(&wifi_cb,
                                 wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(&ipv4_cb,
                                 ipv4_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_cb);

    wifi_callbacks_registered = true;
}

static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();

    if (!iface) {
        dashboard_send_status("no_netif");
        return -ENODEV;
    }

    wifi_events_init_once();

    k_sem_reset(&wifi_connected_sem);
    k_sem_reset(&ipv4_addr_sem);

    wifi_associated = false;
    wifi_ready = false;

    /*
     * Clear any previous half-open connection attempt before retrying.
     * Ignore the return value because disconnect may fail when already idle.
     */
    (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    k_sleep(K_SECONDS(1));

    struct wifi_connect_req_params params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PSK,
        .psk_length = strlen(WIFI_PSK),
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .mfp = WIFI_MFP_OPTIONAL,
    };

    dashboard_send_status("wifi_connecting");

    int err = net_mgmt(NET_REQUEST_WIFI_CONNECT,
                       iface,
                       &params,
                       sizeof(params));

    if (err) {
        char msg[96];
        snprintf(msg, sizeof(msg), "wifi_request_failed_%d", err);
        dashboard_send_status(msg);
        return err;
    }

    err = k_sem_take(&wifi_connected_sem, K_SECONDS(60));

    if (err) {
        dashboard_send_status("wifi_assoc_timeout");
        return -ETIMEDOUT;
    }

    if (!wifi_associated) {
        dashboard_send_status("wifi_failed_not_associated");
        return -EIO;
    }

    err = k_sem_take(&ipv4_addr_sem, K_SECONDS(20));

    if (err) {
        dashboard_send_status("wifi_ipv4_timeout");
        return -ETIMEDOUT;
    }

    dashboard_send_status("wifi_ready");
    return 0;
}
/* ============================================================
 * BLE NUS central
 *
 * Discovery order intentionally follows the previous working
 * CSSE4011 NUS base pattern:
 *
 *   service -> TX char -> TX CCC -> subscribe -> RX char -> ready
 *
 * TX is peripheral-to-central notifications.
 * RX is central-to-peripheral writes.
 * ============================================================ */

enum discovery_state {
    DISCOVER_NUS_SERVICE,
    DISCOVER_NUS_TX_CHAR,
    DISCOVER_NUS_RX_CHAR,
};

static enum discovery_state discovery_state;

static void start_scan(void);
static void start_nus_discovery(struct bt_conn *conn);
static void discover_nus_service(struct bt_conn *conn);
static void discover_nus_tx_char(struct bt_conn *conn);
static void discover_nus_rx_char(struct bt_conn *conn);
static void nus_discovery_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(nus_discovery_work, nus_discovery_work_handler);

static void dashboard_send_status_with_err(const char *prefix, int err)
{
    char msg[128];

    snprintf(msg, sizeof(msg), "%s_%d", prefix, err);
    dashboard_send_status(msg);
}

static int alarm_nus_send(const char *cmd)
{
    if (!alarm_conn || !nus_ready || nus_rx_handle == 0) {
        dashboard_send_status("nus_not_ready");
        return -ENOTCONN;
    }

    int err = bt_gatt_write_without_response(alarm_conn,
                                             nus_rx_handle,
                                             cmd,
                                             strlen(cmd),
                                             false);

    if (err) {
        dashboard_send_status_with_err("nus_write_failed", err);
    }

    return err;
}

static int alarm_send_state(bool enable)
{
    int err = alarm_nus_send(enable ? "ALARM 1\n" : "ALARM 0\n");

    if (err == 0) {
        alarm_state = enable;
        dashboard_send_alert(enable ? "Alarm triggered" : "Alarm cleared");
    }

    return err;
}

static void alarm_update_if_needed(bool required)
{
    if (required != alarm_state) {
        (void)alarm_send_state(required);
    }
}

static uint8_t nus_notify_func(struct bt_conn *conn,
                               struct bt_gatt_subscribe_params *params,
                               const void *data,
                               uint16_t length)
{
    ARG_UNUSED(conn);

    if (!data) {
        params->value_handle = 0;
        params->ccc_handle = 0;
        nus_ready = false;

        dashboard_send_status(alarm_conn ?
                              "nus_notify_stopped_conn_alive" :
                              "nus_notify_stopped_no_conn");

        /* Force a clean reconnect path if the subscription collapses. */
        if (alarm_conn) {
            (void)bt_conn_disconnect(alarm_conn,
                                     BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }

        return BT_GATT_ITER_STOP;
    }

    char msg[256];
    size_t copy_len = MIN(length, sizeof(msg) - 2);

    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';

    if (copy_len == 0 || msg[copy_len - 1] != '\n') {
        msg[copy_len] = '\n';
        msg[copy_len + 1] = '\0';
    }

    serial_send_str(msg);

    return BT_GATT_ITER_CONTINUE;
}

static void report_discovery_not_found(void)
{
    switch (discovery_state) {
    case DISCOVER_NUS_SERVICE:
        dashboard_send_status("nus_service_not_found");
        break;

    case DISCOVER_NUS_TX_CHAR:
        dashboard_send_status("nus_tx_not_found");
        break;

    case DISCOVER_NUS_RX_CHAR:
        dashboard_send_status("nus_rx_not_found");
        break;

    default:
        dashboard_send_status("nus_discovery_failed");
        break;
    }
}

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;

    if (!attr) {
        memset(params, 0, sizeof(*params));

        report_discovery_not_found();
        if (alarm_conn) {
            bt_conn_disconnect(alarm_conn,
                               BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        
        return BT_GATT_ITER_STOP;
    }

  switch (discovery_state) {
    case DISCOVER_NUS_SERVICE: {
        const struct bt_gatt_service_val *service = attr->user_data;
        nus_service_start_handle = attr->handle + 1U;
        nus_service_end_handle = service->end_handle;
        dashboard_send_status("nus_service_found");
        discover_nus_tx_char(conn);
        return BT_GATT_ITER_STOP;
    }

    case DISCOVER_NUS_TX_CHAR: {
        nus_tx_handle = bt_gatt_attr_value_handle(attr);
        nus_tx_ccc_handle = nus_tx_handle + 1U;

        char hmsg[64];
        snprintf(hmsg, sizeof(hmsg), "nus_tx_h%u_ccc_h%u",
                 nus_tx_handle, nus_tx_ccc_handle);
        dashboard_send_status(hmsg);

        memset(&subscribe_params, 0, sizeof(subscribe_params));
        subscribe_params.notify       = nus_notify_func;
        subscribe_params.value        = BT_GATT_CCC_NOTIFY;
        subscribe_params.value_handle = nus_tx_handle;
        subscribe_params.ccc_handle   = nus_tx_ccc_handle;

        err = bt_gatt_subscribe(alarm_conn, &subscribe_params);
        if (err && err != -EALREADY) {
            dashboard_send_status_with_err("nus_subscribe_failed", err);
            if (alarm_conn) {
                bt_conn_disconnect(alarm_conn,
                                   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            }
            return BT_GATT_ITER_STOP;
        }

        dashboard_send_status("nus_subscribed");
        discover_nus_rx_char(alarm_conn);
        return BT_GATT_ITER_STOP;
    }

    case DISCOVER_NUS_RX_CHAR:
        nus_rx_handle = bt_gatt_attr_value_handle(attr);
        nus_ready = true;
        dashboard_send_status("nus_ready");
        return BT_GATT_ITER_STOP;

    default:
        dashboard_send_status("nus_discovery_bad_state");
        return BT_GATT_ITER_STOP;
    }
}

static void discover_nus_service(struct bt_conn *conn)
{
    int err;

    discovery_state = DISCOVER_NUS_SERVICE;

    memset(&discover_params, 0, sizeof(discover_params));

    discover_params.uuid = &nus_service_uuid.uuid;
    discover_params.func = discover_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    err = bt_gatt_discover(conn, &discover_params);

    if (err) {
        dashboard_send_status_with_err("nus_service_discover_start_failed", err);
    }
}

static void discover_nus_tx_char(struct bt_conn *conn)
{
    int err;

    discovery_state = DISCOVER_NUS_TX_CHAR;

    memset(&discover_params, 0, sizeof(discover_params));

    discover_params.uuid = &nus_tx_uuid.uuid;
    discover_params.func = discover_func;
    discover_params.start_handle = nus_service_start_handle;

    /*
     * Match the previous working base-node behaviour: use the full ATT range
     * rather than relying on the service end handle being interpreted exactly
     * as expected by this controller/host combination.
     */
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    err = bt_gatt_discover(conn, &discover_params);

    if (err) {
        dashboard_send_status_with_err("nus_tx_discover_start_failed", err);
    }
}

static void discover_nus_rx_char(struct bt_conn *conn)
{
    int err;

    discovery_state = DISCOVER_NUS_RX_CHAR;

    memset(&discover_params, 0, sizeof(discover_params));

    discover_params.uuid = &nus_rx_uuid.uuid;
    discover_params.func = discover_func;

    /* Continue after the CCC, as in the previous working project. */
    discover_params.start_handle = nus_tx_ccc_handle + 1U;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    err = bt_gatt_discover(conn, &discover_params);

    if (err) {
        dashboard_send_status_with_err("nus_rx_discover_start_failed", err);
    }
}

static void start_nus_discovery(struct bt_conn *conn)
{
    nus_ready = false;
    nus_rx_handle = 0;
    nus_tx_handle = 0;
    nus_tx_ccc_handle = 0;
    nus_service_start_handle = 0;
    nus_service_end_handle = 0;
    ccc_discover_retries = 0;

    dashboard_send_status("nus_discovery_start");

    discover_nus_service(conn);
}

static void nus_discovery_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!alarm_conn) {
        dashboard_send_status("nus_discovery_no_conn");
        return;
    }

    start_nus_discovery(alarm_conn);
}

struct scan_name_ctx {
    bool match;
    char name[40];
};

static bool adv_data_cb(struct bt_data *data, void *user_data)
{
    struct scan_name_ctx *ctx = user_data;

    if (data->type == BT_DATA_NAME_COMPLETE ||
        data->type == BT_DATA_NAME_SHORTENED) {

        size_t len = MIN(data->data_len, sizeof(ctx->name) - 1);

        memcpy(ctx->name, data->data, len);
        ctx->name[len] = '\0';

        if (strcmp(ctx->name, ALARM_NODE_NAME) == 0) {
            ctx->match = true;
            return false;
        }
    }

    return true;
}

static bool adv_is_connectable(uint8_t type)
{
    return type == BT_GAP_ADV_TYPE_ADV_IND ||
           type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND;
}

static void device_found(const bt_addr_le_t *addr,
                         int8_t rssi,
                         uint8_t type,
                         struct net_buf_simple *ad)
{
    if (!adv_is_connectable(type)) {
        return;
    }

    if (alarm_conn) {
        return;
    }

    struct scan_name_ctx ctx = {
        .match = false,
        .name = {0},
    };

    bt_data_parse(ad, adv_data_cb, &ctx);

    if (ctx.name[0] != '\0') {
        char msg[128];

        snprintf(msg, sizeof(msg),
                 "ble_seen_%s_rssi_%d_match_%d",
                 ctx.name,
                 (int)rssi,
                 ctx.match ? 1 : 0);

        dashboard_send_status(msg);
    }

    if (!ctx.match) {
        return;
    }

    dashboard_send_status("ble_target_found");

    int err = bt_le_scan_stop();

    if (err && err != -EALREADY) {
        dashboard_send_status_with_err("ble_scan_stop_failed", err);
    }

    err = bt_conn_le_create(addr,
                            BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM_DEFAULT,
                            &alarm_conn);

    if (err) {
        alarm_conn = NULL;
        dashboard_send_status_with_err("ble_connect_failed", err);
        start_scan();
    }
}

static void start_scan(void)
{
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&scan_param, device_found);

    if (err && err != -EALREADY) {
        dashboard_send_status_with_err("ble_scan_failed", err);
    } else {
        dashboard_send_status("ble_scanning");
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        if (alarm_conn) {
            bt_conn_unref(alarm_conn);
            alarm_conn = NULL;
        }

        dashboard_send_status_with_err("ble_connection_failed", err);
        start_scan();
        return;
    }

    if (!alarm_conn) {
        alarm_conn = bt_conn_ref(conn);
    }

    dashboard_send_status("ble_connected");

    /* Do not block the Bluetooth callback. Start GATT discovery via work. */
    k_work_schedule(&nus_discovery_work, K_MSEC(1500));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);

    nus_ready = false;
    alarm_state = false;
    nus_rx_handle = 0;
    nus_tx_handle = 0;
    nus_tx_ccc_handle = 0;

    k_work_cancel_delayable(&nus_discovery_work);

    if (alarm_conn) {
        bt_conn_unref(alarm_conn);
        alarm_conn = NULL;
    }

    dashboard_send_status_with_err("ble_disconnected_reason", reason);
    start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static int ble_init(void)
{
    int err = bt_enable(NULL);

    if (err) {
        dashboard_send_status_with_err("bt_enable_failed", err);
        return err;
    }

    dashboard_send_status("bt_enabled");
    start_scan();

    return 0;
}
/* ============================================================
 * Dashboard command handling
 * ============================================================ */

static void handle_dashboard_command(const char *line)
{
    char type[24] = {0};
    char target[32] = {0};
    char command[40] = {0};

    (void)extract_string_field(line, "type", type, sizeof(type));
    (void)extract_string_field(line, "target", target, sizeof(target));
    (void)extract_string_field(line, "command", command, sizeof(command));

    if (strcmp(type, "command") != 0) {
        dashboard_send_status("ignored_non_command");
        return;
    }

    if (strcmp(target, "camera") == 0 && strcmp(command, "take_photo") == 0) {
        if (!wifi_ready) {
            dashboard_send_alert("Camera command failed: Wi-Fi not ready");
            return;
        }

        int err = http_post_json(CAMERA_COMMAND_PATH,
                                 "{\"command\":\"take_photo\"}");

        dashboard_send_alert(err == 0 ?
                             "Camera command sent" :
                             "Camera command failed");
        return;
    }

    if ((strcmp(target, "uv_node") == 0 ||
         strcmp(target, "alarm_node") == 0) &&
        strcmp(command, "read_uv") == 0) {
        int err = alarm_nus_send("UV_READ\n");

        dashboard_send_alert(err == 0 ?
                             "UV read requested" :
                             "UV read request failed");
        return;
    }

    if ((strcmp(target, "uv_node") == 0 ||
         strcmp(target, "alarm_node") == 0) &&
        strcmp(command, "alarm_on") == 0) {
        (void)alarm_send_state(true);
        return;
    }

    if ((strcmp(target, "uv_node") == 0 ||
         strcmp(target, "alarm_node") == 0) &&
        strcmp(command, "alarm_off") == 0) {
        (void)alarm_send_state(false);
        return;
    }

    if (strcmp(target, "base_node") == 0 &&
        strcmp(command, "status") == 0) {
        dashboard_send_status("manual_status");
        return;
    }

    dashboard_send_alert("Unknown dashboard command");
}

static void serial_rx_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    char line[MAX_SERIAL_LINE_BYTES];
    size_t idx = 0;

    while (true) {
        unsigned char ch;

        if (uart_poll_in(dashboard_uart, &ch) == 0) {
            if (ch == '\n' || ch == '\r') {
                if (idx > 0) {
                    line[idx] = '\0';
                    handle_dashboard_command(line);
                    idx = 0;
                }
            } else if (idx < sizeof(line) - 1) {
                line[idx++] = (char)ch;
            } else {
                idx = 0;
                dashboard_send_status("serial_command_too_long");
            }
        } else {
            k_sleep(K_MSEC(20));
        }
    }
}

/* ============================================================
 * Wi-Fi manager
 * ============================================================ */

static void wifi_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (true) {
        if (!wifi_ready) {
            int err = wifi_connect();

            if (err) {
                dashboard_send_status("wifi_retrying");
                k_sleep(K_SECONDS(5));
                continue;
            }
        }

        k_sleep(K_SECONDS(10));
    }
}

/* ============================================================
 * Jetson polling
 * ============================================================ */

static void process_jetson_result_once(void)
{
    size_t body_len = 0;
    struct ppe_result result;

    int err = http_get_body(RESULT_PATH,
                            http_body,
                            sizeof(http_body),
                            &body_len);

    if (err) {
        dashboard_send_status("jetson_result_fetch_failed");
        return;
    }

    parse_ppe_result(http_body, &result);

    dashboard_send_ai_result(&result);

    bool alarm_required = ppe_result_requires_alarm(&result);
    alarm_update_if_needed(alarm_required);

    if (result.frame_id != last_forwarded_frame_id) {
        dashboard_send_processed_image_url(&result);
        last_forwarded_frame_id = result.frame_id;
    }
}

static void jetson_poll_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (true) {
        if (wifi_ready) {
            process_jetson_result_once();
        } else {
            dashboard_send_status("wifi_not_ready");
        }

        k_sleep(K_MSEC(JETSON_POLL_PERIOD_MS));
    }
}

static void uv_poll_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (true) {
        if (nus_ready) {
            (void)alarm_nus_send("UV_READ\n");
        }

        k_sleep(K_MSEC(UV_READ_PERIOD_MS));
    }
}

static void heartbeat_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (true) {
        dashboard_send_status("heartbeat");
        k_sleep(K_MSEC(HEARTBEAT_PERIOD_MS));
    }
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    (void)dashboard_uart_init();

    dashboard_send_status("booting");

    k_thread_create(&wifi_thread_data,
                    wifi_stack,
                    K_THREAD_STACK_SIZEOF(wifi_stack),
                    wifi_thread,
                    NULL,
                    NULL,
                    NULL,
                    7,
                    0,
                    K_NO_WAIT);

    (void)ble_init();

    k_thread_create(&serial_rx_thread_data,
                    serial_rx_stack,
                    K_THREAD_STACK_SIZEOF(serial_rx_stack),
                    serial_rx_thread,
                    NULL,
                    NULL,
                    NULL,
                    8,
                    0,
                    K_NO_WAIT);

    k_thread_create(&jetson_poll_thread_data,
                    jetson_poll_stack,
                    K_THREAD_STACK_SIZEOF(jetson_poll_stack),
                    jetson_poll_thread,
                    NULL,
                    NULL,
                    NULL,
                    9,
                    0,
                    K_NO_WAIT);

    k_thread_create(&uv_poll_thread_data,
                    uv_poll_stack,
                    K_THREAD_STACK_SIZEOF(uv_poll_stack),
                    uv_poll_thread,
                    NULL,
                    NULL,
                    NULL,
                    10,
                    0,
                    K_NO_WAIT);

    k_thread_create(&heartbeat_thread_data,
                    heartbeat_stack,
                    K_THREAD_STACK_SIZEOF(heartbeat_stack),
                    heartbeat_thread,
                    NULL,
                    NULL,
                    NULL,
                    11,
                    0,
                    K_NO_WAIT);

    dashboard_send_status("base_node_ready");

    while (true) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}
