#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(uv_alarm_node, LOG_LEVEL_INF);

/* ============================================================
 * Optional alarm GPIO placeholder
 *
 * If you define an alias called "alarm" in your board overlay,
 * this code will use it. Otherwise it will still compile and
 * just log alarm state changes.
 * ============================================================ */

#define ALARM_NODE DT_ALIAS(alarm)

#if DT_NODE_HAS_STATUS(ALARM_NODE, okay)
#define HAS_ALARM_GPIO 1
static const struct gpio_dt_spec alarm_gpio = GPIO_DT_SPEC_GET(ALARM_NODE, gpios);
#else
#define HAS_ALARM_GPIO 0
#endif

/* ============================================================
 * Application state
 * ============================================================ */

static struct bt_conn *current_conn;

static bool alarm_enabled = false;

/* ============================================================
 * NUS command queue
 * ============================================================ */

enum command_type {
    CMD_UV_READ,
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
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* ============================================================
 * Placeholder UV sensor interface
 * ============================================================ */

struct uv_reading {
    int32_t raw;
    int32_t millivolts;
    int32_t uv_index_x100;
};

/*
 * TODO:
 * Replace this function with real UV sensor code.
 *
 * If analog UV sensor:
 *   - enable CONFIG_ADC=y
 *   - create ADC overlay
 *   - use adc_read_dt()
 *
 * If I2C UV sensor:
 *   - enable CONFIG_I2C=y
 *   - create sensor devicetree node
 *   - use i2c_read/i2c_write or sensor API
 */
static int uv_sensor_read(struct uv_reading *out)
{
    if (out == NULL) {
        return -EINVAL;
    }

    /*
     * Placeholder dummy values.
     * Replace these with actual sensor values.
     */
    out->raw = 1234;
    out->millivolts = 850;
    out->uv_index_x100 = 245;  /* 2.45 UV index */

    return 0;
}

/* ============================================================
 * Placeholder alarm/buzzer interface
 * ============================================================ */

static int alarm_init(void)
{
#if HAS_ALARM_GPIO
    if (!gpio_is_ready_dt(&alarm_gpio)) {
        LOG_ERR("Alarm GPIO device not ready");
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&alarm_gpio, GPIO_OUTPUT_INACTIVE);

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
    gpio_pin_set_dt(&alarm_gpio, enabled ? 1 : 0);
#endif

    LOG_INF("Alarm %s", enabled ? "ON" : "OFF");
}

/* ============================================================
 * NUS send helper
 * ============================================================ */

static void nus_send_text(const char *text)
{
    if (current_conn == NULL) {
        LOG_WRN("No BLE connection; cannot send NUS message");
        return;
    }

    int err = bt_nus_send(current_conn, text, strlen(text));

    if (err) {
        LOG_WRN("bt_nus_send failed: %d", err);
    }
}

/* ============================================================
 * Response helpers
 * ============================================================ */

static void send_uv_response(void)
{
    struct uv_reading reading;
    char msg[160];

    int err = uv_sensor_read(&reading);

    if (err) {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"uv_error\",\"err\":%d}\n",
                 err);

        nus_send_text(msg);
        return;
    }

    snprintf(msg, sizeof(msg),
             "{\"type\":\"uv_data\","
             "\"raw\":%ld,"
             "\"mv\":%ld,"
             "\"uv_x100\":%ld,"
             "\"alarm\":%s}\n",
             (long)reading.raw,
             (long)reading.millivolts,
             (long)reading.uv_index_x100,
             alarm_enabled ? "true" : "false");

    nus_send_text(msg);

    LOG_INF("Sent UV response: raw=%ld mv=%ld uv_x100=%ld",
            (long)reading.raw,
            (long)reading.millivolts,
            (long)reading.uv_index_x100);
}

static void send_alarm_ack(bool enabled)
{
    char msg[96];

    snprintf(msg, sizeof(msg),
             "{\"type\":\"alarm_ack\",\"alarm\":%s}\n",
             enabled ? "true" : "false");

    nus_send_text(msg);
}

/* ============================================================
 * Command processing
 * ============================================================ */

static void process_command(const struct app_command *cmd)
{
    switch (cmd->type) {
    case CMD_UV_READ:
        send_uv_response();
        break;

    case CMD_ALARM_SET:
        alarm_set(cmd->alarm_value);
        send_alarm_ack(cmd->alarm_value);
        break;

    case CMD_UNKNOWN:
    default:
        nus_send_text("{\"type\":\"error\",\"msg\":\"unknown_command\"}\n");
        break;
    }
}

static void command_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct app_command cmd;

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

    /*
     * Expected base-node commands:
     *
     *   UV_READ
     *   ALARM 1
     *   ALARM 0
     *
     * Extra accepted forms:
     *
     *   UV?
     *   ALARM_ON
     *   ALARM_OFF
     */

    if (strcmp(text, "UV_READ") == 0 || strcmp(text, "UV?") == 0) {
        cmd.type = CMD_UV_READ;
        return cmd;
    }

    if (strcmp(text, "ALARM 1") == 0 || strcmp(text, "ALARM_ON") == 0) {
        cmd.type = CMD_ALARM_SET;
        cmd.alarm_value = true;
        return cmd;
    }

    if (strcmp(text, "ALARM 0") == 0 || strcmp(text, "ALARM_OFF") == 0) {
        cmd.type = CMD_ALARM_SET;
        cmd.alarm_value = false;
        return cmd;
    }

    return cmd;
}

static void nus_received_cb(struct bt_conn *conn,
                            const void *data,
                            uint16_t len)
{
    ARG_UNUSED(conn);

    char rx_text[80];

    size_t copy_len = MIN(len, sizeof(rx_text) - 1);
    memcpy(rx_text, data, copy_len);
    rx_text[copy_len] = '\0';

    /*
     * Strip newline and carriage return.
     */
    for (size_t i = 0; i < copy_len; i++) {
        if (rx_text[i] == '\n' || rx_text[i] == '\r') {
            rx_text[i] = '\0';
            break;
        }
    }

    LOG_INF("NUS RX: %s", rx_text);

    struct app_command cmd = parse_command(rx_text);

    int err = k_msgq_put(&command_queue, &cmd, K_NO_WAIT);

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
 * BLE connection callbacks
 * ============================================================ */

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed: %u", err);
        return;
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

    err = bt_nus_init(&nus_cb);

    if (err) {
        LOG_ERR("NUS init failed: %d", err);
        return err;
    }

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                          ad, ARRAY_SIZE(ad),
                          sd, ARRAY_SIZE(sd));

    if (err) {
        LOG_ERR("Advertising failed: %d", err);
        return err;
    }

    LOG_INF("Advertising as %s", DEVICE_NAME);

    return 0;
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    LOG_INF("UV alarm NUS node starting");

    int err;

    k_work_init(&command_work, command_work_handler);

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

    LOG_INF("UV alarm NUS node ready");

    return 0;
}