#include "display_idle.h"

#include "settings/settings_display_screen.h"
#include <bsp/display.h>
#include <esp_log.h>
#include <lvgl.h>

static const char* TAG = "display_idle";

static constexpr uint32_t IDLE_TIMEOUT_MS = 5 * 60 * 1000;
static constexpr int IDLE_BRIGHTNESS = 10;
static constexpr uint32_t CHECK_INTERVAL_MS = 1000;

static bool s_initialized = false;
static bool s_dimmed = false;
static uint32_t s_last_activity_ms = 0;

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
    if (display_screen_get_brightness() <= IDLE_BRIGHTNESS) {
        s_last_activity_ms = lv_tick_get();
        s_dimmed = false;
        return;
    }

    esp_err_t err = bsp_display_brightness_set(IDLE_BRIGHTNESS);
    if (err == ESP_OK) {
        s_dimmed = true;
        ESP_LOGI(TAG, "Display dimmed to %d%%", IDLE_BRIGHTNESS);
    } else {
        ESP_LOGE(TAG, "Failed to dim display: %s", esp_err_to_name(err));
    }
}

bool display_idle_handle_activity(void)
{
    s_last_activity_ms = lv_tick_get();
    if (!s_dimmed) return false;

    restore_saved_brightness();
    s_dimmed = false;
    ESP_LOGI(TAG, "Display restored after user activity");
    return true;
}

bool display_idle_is_dimmed(void)
{
    return s_dimmed;
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
    if (!s_dimmed && lv_tick_elaps(s_last_activity_ms) >= IDLE_TIMEOUT_MS) {
        display_idle_force_dim();
    }
}

void display_idle_init(void)
{
    if (s_initialized) return;

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
    ESP_LOGI(TAG, "Idle dimming enabled after %lu ms", (unsigned long)IDLE_TIMEOUT_MS);
}
