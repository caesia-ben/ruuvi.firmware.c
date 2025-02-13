/**
 * @file app_heartbeat.c
 * @author Otso Jousimaa <otso@ojousima.net>
 * @date 2020-06-17
 * @copyright Ruuvi Innovations Ltd, License BSD-3-Clause.
 */

#include "app_config.h"
#include "app_comms.h"
#include "app_dataformats.h"
#include "app_heartbeat.h"
#include "app_log.h"
#include "app_sensor.h"
#include "ruuvi_driver_error.h"
#include "ruuvi_driver_sensor.h"
#include "ruuvi_endpoint_5.h"
#include "ruuvi_interface_communication.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_scheduler.h"
#include "ruuvi_interface_timer.h"
#include "ruuvi_interface_watchdog.h"
#include "ruuvi_task_adc.h"
#include "ruuvi_task_advertisement.h"
#include "ruuvi_task_gatt.h"
#include "ruuvi_task_nfc.h"

#define U8_MASK (0xFFU)
#define APP_DF_3_ENABLED 0
#define APP_DF_5_ENABLED 1
#define APP_DF_8_ENABLED 0
#define APP_DF_FA_ENABLED 0

static uint32_t heartbeat_gatt_interval_dynamic_ms;
static uint32_t heartbeat_interval_ms;
static int64_t heartbeat_gatt_live_until = 0;

static int64_t last_accelerometer_active_time_ms;

static int64_t last_hb_ms;
static bool timer_running = false;

static int64_t last_led_flash_ms;

static ri_timer_id_t heart_timer; //!< Timer for updating data.

static uint64_t last_heartbeat_timestamp_ms; //!< Timestamp for heartbeat refresh.

static app_dataformat_t m_dataformat_state; //!< State of heartbeat.

static app_dataformats_t m_dataformats_enabled =
{
    .DF_3  = APP_DF_3_ENABLED,
    .DF_5  = APP_DF_5_ENABLED,
    .DF_8  = APP_DF_8_ENABLED,
    .DF_FA = APP_DF_FA_ENABLED
}; //!< Flags of enabled formats

static rd_status_t send_adv (ri_comm_message_t * const p_msg)
{
    rd_status_t err_code = RD_SUCCESS;
    const uint8_t repeat_count = app_comms_bleadv_send_count_get();

    if (APP_COMM_ADV_DISABLE != repeat_count)
    {
        if (APP_COMM_ADV_REPEAT_FOREVER == repeat_count)
        {
            p_msg->repeat_count = RI_COMM_MSG_REPEAT_FOREVER;
        }
        else
        {
            p_msg->repeat_count = repeat_count;
        }

        err_code |= rt_adv_send_data (p_msg);
    }
    else
    {
        rt_adv_stop();
    }

    return err_code;
}

static void check_accel_is_active(rd_sensor_data_t *const data) {

    float x = rd_sensor_data_parse (data, RD_SENSOR_ACC_X_FIELD);
    float y = rd_sensor_data_parse (data, RD_SENSOR_ACC_Y_FIELD);
    float z = rd_sensor_data_parse (data, RD_SENSOR_ACC_Z_FIELD);

    if (x > 1.5f || y > 1.5f || z > 1.5f) {
        last_accelerometer_active_time_ms = ri_rtc_millis();
    }
}

/**
 * @brief When timer triggers, schedule reading sensors and sending data.
 *
 * @param[in] p_context Always NULL.
 */
#ifndef CEEDLING
static
#endif
void heartbeat (void * p_event, uint16_t event_size)
{
    bool gatt_active = false;
    ri_comm_message_t msg = {0};
    rd_status_t err_code = RD_SUCCESS;
    bool heartbeat_ok = false;
    rd_sensor_data_t data = { 0 };
    size_t buffer_len = RI_COMM_MESSAGE_MAX_LENGTH;
    data.fields = app_sensor_available_data();
    float data_values[rd_sensor_data_fieldcount (&data)];
    data.data = data_values;
    app_sensor_get (&data);
    m_dataformat_state = app_dataformat_next (m_dataformats_enabled, m_dataformat_state);
    app_dataformat_encode (msg.data, &buffer_len, &data, m_dataformat_state);
    msg.data_length = (uint8_t) buffer_len;
    // Sensor read takes a long while, indicate activity once data is read.
    if ((last_led_flash_ms + 1950) < ri_rtc_millis()) {
        last_led_flash_ms = ri_rtc_millis();
        // app_led_activity_signal (true);
        err_code = send_adv (&msg);
        // Advertising should always be successful
        RD_ERROR_CHECK (err_code, ~RD_ERROR_FATAL);
    }

    if (RD_SUCCESS == err_code)
    {
        heartbeat_ok = true;
    }

    // Cut endpoint data to fit into GATT msg.
    msg.data_length = 18;
    // Gatt Link layer takes care of delivery.
    msg.repeat_count = 1;
    // err_code = app_comms_blocking_send (rt_gatt_send_asynchronous, &msg);
    err_code = rt_gatt_send_asynchronous(&msg);

    if (RD_SUCCESS == err_code)
    {
        heartbeat_ok = true;
    }

    gatt_active = err_code == RD_SUCCESS;

    // Restore original message length for NFC
    msg.data_length = (uint8_t) buffer_len;
    // err_code = rt_nfc_send (&msg);

    if (RD_SUCCESS == err_code)
    {
        heartbeat_ok = true;
    }

    if (heartbeat_ok)
    {
        ri_watchdog_feed();
        last_heartbeat_timestamp_ms = ri_rtc_millis();
    }

    last_hb_ms = ri_rtc_millis();

    // check_accel_is_active(&data);
    // uint32_t new_heartbeat_interval_ms;
    // if (gatt_active) {
    //     new_heartbeat_interval_ms = heartbeat_gatt_interval_dynamic_ms;
    // }
    // else {
    //     new_heartbeat_interval_ms = APP_HEARTBEAT_INTERVAL_MS;
    // }

    // if (heartbeat_interval_ms != new_heartbeat_interval_ms) {
    //     heartbeat_interval_ms = new_heartbeat_interval_ms;
    //     ri_timer_stop (heart_timer);
    //     ri_timer_start (heart_timer, heartbeat_interval_ms, NULL);
    // }

    // Turn LED off before starting lengthy flash operations
    // app_led_activity_signal (false);
    // err_code = app_log_process (&data);
    RD_ERROR_CHECK (err_code, ~RD_ERROR_FATAL);
}

/**
 * @brief When timer triggers, schedule reading sensors and sending data.
 *
 * @param[in] p_context Always NULL.
 */
#ifndef CEEDLING
static
#endif
void schedule_heartbeat_isr (void * const p_context)
{
    ri_scheduler_event_put (NULL, 0U, &heartbeat);
}

rd_status_t app_heartbeat_set_gatt_interval_ms(uint32_t interval_ms) {

    ri_timer_stop (heart_timer);
    timer_running = false;
    heartbeat_gatt_interval_dynamic_ms = interval_ms;
    heartbeat_gatt_live_until = 30000 + ri_rtc_millis();
    return RD_SUCCESS;
}

rd_status_t app_heartbeat_init (void)
{
    rd_status_t err_code = RD_SUCCESS;
    heartbeat_interval_ms = APP_HEARTBEAT_INTERVAL_MS;
    heartbeat_gatt_interval_dynamic_ms = APP_HEARTBEAT_INTERVAL_MS;
    timer_running = false;
    last_hb_ms = ri_rtc_millis();
    last_accelerometer_active_time_ms = -1 * (1000 * 60 * 9);
    last_led_flash_ms = ri_rtc_millis();
    heartbeat_gatt_live_until = ri_rtc_millis();

    if ( (!ri_timer_is_init()) || (!ri_scheduler_is_init()))
    {
        err_code |= RD_ERROR_INVALID_STATE;
    }
    else
    {
        err_code |= ri_timer_create (&heart_timer, RI_TIMER_MODE_REPEATED,
                                     &schedule_heartbeat_isr);

        if (RD_SUCCESS == err_code)
        {
            timer_running = true;
            err_code |= ri_timer_start (heart_timer, heartbeat_interval_ms, NULL);
        }
    }

    return err_code;
}

bool app_heartbeat_should_sleep (void) {
    if (heartbeat_gatt_live_until < ri_rtc_millis()) {
        if (!timer_running) {
            ri_timer_start (heart_timer, heartbeat_interval_ms, NULL);
            timer_running = true;
        }
        return true;
    }
    
    if ((last_hb_ms + heartbeat_gatt_interval_dynamic_ms) < ri_rtc_millis())
    {
        // app_led_activity_signal (true);
        heartbeat (NULL, 0);
        // app_led_activity_signal (false);
        // app_led_error_signal(false);
    }
    // else
    // {
    //     heartbeat (NULL, 0);
    // }

    return false;
}

rd_status_t app_heartbeat_start (void)
{
    rd_status_t err_code = RD_SUCCESS;

    if (NULL == heart_timer)
    {
        err_code |= RD_ERROR_INVALID_STATE;
    }
    else
    {
        heartbeat (NULL, 0);
        err_code |= ri_timer_start (heart_timer, heartbeat_interval_ms, NULL);
    }

    return err_code;
}

rd_status_t app_heartbeat_stop (void)
{
    rd_status_t err_code = RD_SUCCESS;

    if (NULL == heart_timer)
    {
        err_code |= RD_ERROR_INVALID_STATE;
    }
    else
    {
        err_code |= ri_timer_stop (heart_timer);
    }

    return err_code;
}

bool app_heartbeat_overdue (void)
{
    return ri_rtc_millis() > (last_heartbeat_timestamp_ms +
                              APP_HEARTBEAT_OVERDUE_INTERVAL_MS);
}

#ifdef CEEDLING
// Give CEEDLING a handle to state of module.
ri_timer_id_t * get_heart_timer (void)
{
    return &heart_timer;
}
#endif
