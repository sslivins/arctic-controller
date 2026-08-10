#include "display_idle.h"

#include "settings/settings_display_screen.h"
#include <bsp/display.h>
#include <esp_log.h>
#include <lvgl.h>
#include <nvs.h>

static const char* TAG = "display_idle";

static constexpr int IDLE_BRIGHTNESS = 10;
static constexpr uint32_t CHECK_INTERVAL_MS = 1000;
static constexpr uint8_t DEFAULT_DIM_MINUTES = 1;
static constexpr uint8_t DEFAULT_OFF_MINUTES = 4;
static constexpr uint8_t MAX_TIMEOUT_MINUTES = 5;
static constexpr const char* NVS_NAMESPACE = "display_cfg";
static constexpr const char* NVS_KEY_DIM_MINUTES = "dim_min";
static constexpr const char* NVS_KEY_OFF_MINUTES = "off_min";

static bool s_initialized = false;
static bool s_dimmed = false;
static bool s_off = false;
static uint32_t s_last_activity_ms = 0;
static uint32_t s_dimmed_since_ms = 0;
static uint8_t s_dim_minutes = DEFAULT_DIM_MINUTES;
static uint8_t s_off_minutes = DEFAULT_OFF_MINUTES;

static uint32_t minutes_to_ms(uint8_t minutes)
{
    return static_cast<uint32_t>(minutes) * 60U * 1000U;
}

static void load_timeouts()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    uint8_t value = 0;
    if (nvs_get_u8(nvs, NVS_KEY_DIM_MINUTES, &value) == ESP_OK &&
        value <= MAX_TIMEOUT_MINUTES) {
        s_dim_minutes = value;
    }
    if (nvs_get_u8(nvs, NVS_KEY_OFF_MINUTES, &value) == ESP_OK &&
        value <= MAX_TIMEOUT_MINUTES) {
        s_off_minutes = value;
    }
    nvs_close(nvs);
}

static void restore_saved_brightness()
{
    const int brightness = display_screen_get_brightness();
    esp_err_t err = bsp_display_brightness_set(brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore brightness: %s", esp_err_to_name(err));
    }
}

void display_idle_force_dim(void)
{
    const int dim_brightness =
        display_screen_get_brightness() < IDLE_BRIGHTNESS
            ? display_screen_get_brightness()
            : IDLE_BRIGHTNESS;
    esp_err_t err = bsp_display_brightness_set(dim_brightness);
    if (err == ESP_OK) {
        s_dimmed = true;
        s_off = false;
        s_dimmed_since_ms = lv_tick_get();
        ESP_LOGI(TAG, "Display dimmed to %d%%", dim_brightness);
    } else {
        ESP_LOGE(TAG, "Failed to dim display: %s", esp_err_to_name(err));
    }
}

void display_idle_force_off(void)
{
    esp_err_t err = bsp_display_brightness_set(0);
    if (err == ESP_OK) {
        s_dimmed = false;
        s_off = true;
        ESP_LOGI(TAG, "Display backlight turned off");
    } else {
        ESP_LOGE(TAG, "Failed to turn off display: %s", esp_err_to_name(err));
    }
}

bool display_idle_handle_activity(void)
{
    s_last_activity_ms = lv_tick_get();
    if (!s_dimmed && !s_off) return false;

    restore_saved_brightness();
    s_dimmed = false;
    s_off = false;
    ESP_LOGI(TAG, "Display restored after user activity");
    return true;
}

bool display_idle_is_dimmed(void)
{
    return s_dimmed;
}

bool display_idle_is_off(void)
{
    return s_off;
}

uint8_t display_idle_get_dim_minutes(void)
{
    return s_dim_minutes;
}

uint8_t display_idle_get_off_minutes(void)
{
    return s_off_minutes;
}

bool display_idle_set_timeouts(uint8_t dim_minutes, uint8_t off_minutes)
{
    if (dim_minutes > MAX_TIMEOUT_MINUTES ||
        off_minutes > MAX_TIMEOUT_MINUTES) {
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open display settings: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(nvs, NVS_KEY_DIM_MINUTES, dim_minutes);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY_OFF_MINUTES, off_minutes);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save idle timeouts: %s", esp_err_to_name(err));
        return false;
    }

    s_dim_minutes = dim_minutes;
    s_off_minutes = off_minutes;
    display_idle_handle_activity();
    s_last_activity_ms = lv_tick_get();
    ESP_LOGI(TAG, "Idle timeouts updated: dim=%u min, off=%u min after dim",
             (unsigned)dim_minutes, (unsigned)off_minutes);
    return true;
}

static void input_event_cb(lv_event_t* event)
{
    if (!display_idle_handle_activity()) return;

    lv_indev_t* indev = static_cast<lv_indev_t*>(lv_event_get_current_target(event));
    if (indev) {
        lv_indev_stop_processing(indev);
        lv_indev_wait_release(indev);
    }
}

static void idle_timer_cb(lv_timer_t*)
{
    if (!s_dimmed && !s_off && s_dim_minutes > 0 &&
        lv_tick_elaps(s_last_activity_ms) >= minutes_to_ms(s_dim_minutes)) {
        display_idle_force_dim();
    } else if (s_dimmed && s_off_minutes > 0 &&
               lv_tick_elaps(s_dimmed_since_ms) >= minutes_to_ms(s_off_minutes)) {
        display_idle_force_off();
    }
}

void display_idle_init(void)
{
    if (s_initialized) return;

    load_timeouts();
    s_last_activity_ms = lv_tick_get();
    for (lv_indev_t* indev = lv_indev_get_next(nullptr);
         indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_PRESSED, nullptr);
        }
    }
    lv_timer_create(idle_timer_cb, CHECK_INTERVAL_MS, nullptr);
    s_initialized = true;
    ESP_LOGI(TAG, "Idle display stages: dim=%u min, off=%u min after dim",
             (unsigned)s_dim_minutes, (unsigned)s_off_minutes);
}
