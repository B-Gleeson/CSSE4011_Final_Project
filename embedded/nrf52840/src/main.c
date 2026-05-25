#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(humidity_alarm_node, LOG_LEVEL_INF);

/* ============================================================
 * DHT11 / XC4520 humidity sensor
 *
 * Required overlay alias:
 *
 * aliases {
 *     dht0 = &humidity_sensor;
 * };
 * ============================================================ */

#define DHT_NODE DT_ALIAS(dht0)

#if !DT_NODE_HAS_STATUS(DHT_NODE, okay)
#error "No enabled dht0 devicetree alias found. Add the XC4520/DHT11 node to your overlay."
#endif

static const struct device *const dht_dev = DEVICE_DT_GET(DHT_NODE);

/* ============================================================
 * Optional alarm GPIO placeholder
 *
 * If you define an alias called "alarm" in your board overlay,
 * this code will use it. Otherwise it will still compile and
 * only log alarm state changes.
 * ============================================================ */

#define ALARM_NODE DT_ALIAS(alarm)

#if DT_NODE_HAS_STATUS(ALARM_NODE, okay)
#define HAS_ALARM_GPIO 1
static const struct gpio_dt_spec alarm_gpio =
    GPIO_DT_SPEC_GET(ALARM_NODE, gpios);
#else
#define HAS_ALARM_GPIO 0
#endif

/* ============================================================
 * Application state
 * ============================================================ */

static struct bt_conn *current_conn;
static bool alarm_enabled;

/* ============================================================
 * NUS command queue
 * ============================================================ */

enum command_type {
    CMD_HUMIDITY_READ,
    CMD_ALARM_SET,
    CMD_UNKNOWN,
};

struct app_command {
    enum command_type type;
    bool alarm_value;
};

K_MSGQ_DEFINE(command_queue, sizeof(struct app_command), 8, 4);

static struct k_work command_work;

/* ============================================================
 * BLE advertising data
 * ============================================================ */

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* ============================================================
 * XC4520 / DHT11 sensor interface
 *
 * Values are returned as x100 integers:
 *
 * humidity_x100       = 5432 means 54.32 %RH
 * temperature_c_x100  = 2380 means 23.80 deg C
 *
 * The DHT11 itself normally has coarse resolution, but this
 * format keeps the JSON structure clean and avoids float printf.
 * ============================================================ */

struct humidity_reading {
    int32_t humidity_x100;
    int32_t temperature_c_x100;
};

static int32_t sensor_value_to_x100(const struct sensor_value *value)
{
    return (int32_t)((value->val1 * 100) + (value->val2 / 10000));
}

static int humidity_sensor_init(void)
{
    if (!device_is_ready(dht_dev)) {
        LOG_ERR("Humidity sensor device not ready");
        return -ENODEV;
    }

    LOG_INF("XC4520/DHT11 humidity sensor initialized");
    return 0;
}

static int humidity_sensor_read(struct humidity_reading *out)
{
    struct sensor_value humidity;
    struct sensor_value temperature;
    int err;

    if (out == NULL) {
        return -EINVAL;
    }

    err = sensor_sample_fetch(dht_dev);
    if (err) {
        LOG_ERR("DHT sample fetch failed: %d", err);
        return err;
    }

    err = sensor_channel_get(dht_dev, SENSOR_CHAN_HUMIDITY, &humidity);
    if (err) {
        LOG_ERR("Humidity channel read failed: %d", err);
        return err;
    }

    err = sensor_channel_get(dht_dev,
                             SENSOR_CHAN_AMBIENT_TEMP,
                             &temperature);
    if (err) {
        LOG_ERR("Temperature channel read failed: %d", err);
        return err;
    }

    out->humidity_x100 = sensor_value_to_x100(&humidity);
    out->temperature_c_x100 = sensor_value_to_x100(&temperature);

    return 0;
}

/* ============================================================
 * Alarm / buzzer interface
 * ============================================================ */

static int alarm_init(void)
{
#if HAS_ALARM_GPIO
    int err;

    if (!gpio_is_ready_dt(&alarm_gpio)) {
        LOG_ERR("Alarm GPIO device not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&alarm_gpio, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure alarm GPIO: %d", err);
        return err;
    }

    LOG_INF("Alarm GPIO initialized");
#else
    LOG_WRN("No alarm GPIO alias found. Alarm is placeholder only.");
#endif

    return 0;
}

static void alarm_set(bool enabled)
{
    alarm_enabled = enabled;

#if HAS_ALARM_GPIO
    int err = gpio_pin_set_dt(&alarm_gpio, enabled ? 1 : 0);

    if (err) {
        LOG_ERR("Failed to set alarm GPIO: %d", err);
    }
#endif

    LOG_INF("Alarm %s", enabled ? "ON" : "OFF");
}

/* ============================================================
 * NUS send helper
 * ============================================================ */

static void nus_send_text(const char *text)
{
    int err;

    if (current_conn == NULL) {
        LOG_WRN("No BLE connection; cannot send NUS message");
        return;
    }

    err = bt_nus_send(current_conn, text, strlen(text));
    if (err) {
        LOG_WRN("bt_nus_send failed: %d", err);
    }
}

/* ============================================================
 * Response helpers
 * ============================================================ */

static void send_humidity_response(void)
{
    struct humidity_reading reading;
    char msg[24];
    int err;

    err = humidity_sensor_read(&reading);

    if (err) {
        /*
         * Compact BLE response for the base node:
         * E,<error_code>\n
         *
         * Keep this below the default NUS notification payload size.
         */
        snprintf(msg, sizeof(msg), "E,%d\n", err);
        nus_send_text(msg);

        LOG_ERR("DHT read failed: %d", err);
        return;
    }

    /*
     * Compact BLE response for the base node:
     * H,<humidity_x100>,<temperature_c_x100>,<alarm>\n
     *
     * Example: H,5400,2310,0
     */
    snprintf(msg, sizeof(msg),
             "H,%ld,%ld,%d\n",
             (long)reading.humidity_x100,
             (long)reading.temperature_c_x100,
             alarm_enabled ? 1 : 0);

    nus_send_text(msg);

    LOG_INF("Sent compact humidity response: %s", msg);
}

static void send_alarm_ack(bool enabled)
{
    char msg[8];

    /*
     * Compact BLE packet:
     * A,1\n = alarm enabled
     * A,0\n = alarm disabled
     */
    snprintf(msg, sizeof(msg), "A,%d\n", enabled ? 1 : 0);

    nus_send_text(msg);
}

/* ============================================================
 * Command processing
 * ============================================================ */

static void process_command(const struct app_command *cmd)
{
    switch (cmd->type) {
    case CMD_HUMIDITY_READ:
        //nus_send_text("H,5400,2300,0\n");
        //LOG_INF("DIAG: HUMIDITY_READ received, response suppressed");
        send_humidity_response();
        break;

    case CMD_ALARM_SET:
        alarm_set(cmd->alarm_value);
        //LOG_INF("DIAG: ALARM command received, ACK suppressed");
        send_alarm_ack(cmd->alarm_value);
        break;

    case CMD_UNKNOWN:
    default:
        nus_send_text("{\"type\":\"error\",\"msg\":\"unknown_command\"}\n");
        //LOG_INF("DIAG: unknown command suppressed");
        break;
    }
}

static void command_work_handler(struct k_work *work)
{
    struct app_command cmd;

    ARG_UNUSED(work);

    while (k_msgq_get(&command_queue, &cmd, K_NO_WAIT) == 0) {
        process_command(&cmd);
    }
}

/* ============================================================
 * NUS RX command parser
 * ============================================================ */

static struct app_command parse_command(const char *text)
{
    struct app_command cmd = {
        .type = CMD_UNKNOWN,
        .alarm_value = false,
    };

    if (strcmp(text, "HUMIDITY_READ") == 0 ||
        strcmp(text, "HUMIDITY?") == 0) {
        cmd.type = CMD_HUMIDITY_READ;
        return cmd;
    }

    if (strcmp(text, "ALARM 1") == 0 ||
        strcmp(text, "ALARM_ON") == 0) {
        cmd.type = CMD_ALARM_SET;
        cmd.alarm_value = true;
        return cmd;
    }

    if (strcmp(text, "ALARM 0") == 0 ||
        strcmp(text, "ALARM_OFF") == 0) {
        cmd.type = CMD_ALARM_SET;
        cmd.alarm_value = false;
        return cmd;
    }

    return cmd;
}

static void nus_received_cb(struct bt_conn *conn,
                            const uint8_t *data,
                            uint16_t len)
{
    struct app_command cmd;
    char rx_text[80];
    size_t copy_len;
    int err;

    ARG_UNUSED(conn);

    copy_len = MIN((size_t)len, sizeof(rx_text) - 1);
    memcpy(rx_text, data, copy_len);
    rx_text[copy_len] = '\0';

    for (size_t i = 0; i < copy_len; i++) {
        if (rx_text[i] == '\n' || rx_text[i] == '\r') {
            rx_text[i] = '\0';
            break;
        }
    }

    LOG_INF("NUS RX: %s", rx_text);

    cmd = parse_command(rx_text);

    err = k_msgq_put(&command_queue, &cmd, K_NO_WAIT);
    if (err) {
        LOG_WRN("Command queue full");
        nus_send_text("{\"type\":\"error\",\"msg\":\"command_queue_full\"}\n");
        return;
    }

    k_work_submit(&command_work);
}

static struct bt_nus_cb nus_cb = {
    .received = nus_received_cb,
};

/* ============================================================
 * BLE advertising restart
 * ============================================================ */

static int start_advertising(void);

static void restart_adv_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)start_advertising();
}

K_WORK_DEFINE(restart_adv_work, restart_adv_work_handler);

static int start_advertising(void)
{
    int err;

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                          ad, ARRAY_SIZE(ad),
                          sd, ARRAY_SIZE(sd));

    if (err == -EALREADY) {
        return 0;
    }

    if (err) {
        LOG_ERR("Advertising failed: %d", err);
        return err;
    }

    LOG_INF("Advertising as %s", DEVICE_NAME);
    return 0;
}

/* ============================================================
 * BLE connection callbacks
 * ============================================================ */

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed: %u", err);
        return;
    }

    if (current_conn != NULL) {
        bt_conn_unref(current_conn);
    }

    current_conn = bt_conn_ref(conn);

    LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);

    LOG_INF("BLE disconnected, reason=%u", reason);

    if (current_conn != NULL) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    k_work_submit(&restart_adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ============================================================
 * BLE init
 * ============================================================ */

static int ble_init(void)
{
    int err;

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return err;
    }

    LOG_INF("Bluetooth initialized");

    err = bt_nus_cb_register(&nus_cb, NULL);
    if (err) {
        LOG_ERR("NUS callback register failed: %d", err);
        return err;
    }

    err = start_advertising();
    if (err) {
        return err;
    }

    return 0;
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    int err;

    LOG_INF("Humidity alarm NUS node starting");

    k_work_init(&command_work, command_work_handler);

    err = humidity_sensor_init();
    if (err) {
        LOG_ERR("Humidity sensor init failed: %d", err);
        return err;
    }

    err = alarm_init();
    if (err) {
        LOG_ERR("Alarm init failed: %d", err);
        return err;
    }

    err = ble_init();
    if (err) {
        LOG_ERR("BLE init failed: %d", err);
        return err;
    }

    LOG_INF("Humidity alarm NUS node ready");

    while (true) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}