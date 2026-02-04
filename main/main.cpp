/*
 * Arctic Heat Pump Controller
 * Main entry point for ESP32-P4 Tab5
 */
#include <stdio.h>
#include <bsp/m5stack_tab5.h>
#include <lvgl.h>
#include <mooncake_log.h>

static const char* TAG = "main";

// Forward declaration
void create_ui(void);

extern "C" void app_main(void)
{
    mclog::tagInfo(TAG, "Arctic Heat Pump Controller Starting...");

    // Initialize I2C (required for IO expander and other peripherals)
    mclog::tagInfo(TAG, "Initializing I2C...");
    bsp_i2c_init();

    // Initialize IO expander (required for display control)
    mclog::tagInfo(TAG, "Initializing IO expander...");
    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    bsp_io_expander_pi4ioe_init(i2c_bus_handle);

    // Reset touch panel
    bsp_reset_tp();

    // Initialize Tab5 BSP (display, touch, etc.)
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
        }
    };
    display_cfg.lvgl_port_cfg.task_priority = 5;
    
    lv_display_t* display = bsp_display_start_with_config(&display_cfg);
    if (display == NULL) {
        mclog::tagError(TAG, "Failed to initialize display!");
        return;
    }

    // Set rotation and turn on backlight
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    bsp_display_backlight_on();

    mclog::tagInfo(TAG, "Display initialized: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);

    // Create UI
    bsp_display_lock(0);
    create_ui();
    bsp_display_unlock();

    mclog::tagInfo(TAG, "UI Created - Hello World!");

    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void create_ui(void)
{
    // Get the active screen
    lv_obj_t* scr = lv_scr_act();
    
    // Set background color
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    
    // Create title label
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Arctic Heat Pump");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00d4ff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    // Create subtitle
    lv_obj_t* subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Controller v0.1");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 140);

    // Create a container for the main content
    lv_obj_t* container = lv_obj_create(scr);
    lv_obj_set_size(container, 600, 400);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x16213e), LV_PART_MAIN);
    lv_obj_set_style_border_color(container, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(container, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 30, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Temperature display (placeholder)
    lv_obj_t* temp_label = lv_label_create(container);
    lv_label_set_text(temp_label, "-- °C");
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* temp_desc = lv_label_create(container);
    lv_label_set_text(temp_desc, "Current Temperature");
    lv_obj_set_style_text_font(temp_desc, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_desc, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(temp_desc, LV_ALIGN_TOP_MID, 0, 90);

    // Status label
    lv_obj_t* status = lv_label_create(container);
    lv_label_set_text(status, "Status: Initializing...");
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(0xffcc00), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 30);

    // WiFi Config button (placeholder - doesn't do anything yet)
    lv_obj_t* btn_wifi = lv_btn_create(container);
    lv_obj_set_size(btn_wifi, 200, 60);
    lv_obj_align(btn_wifi, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x1a5276), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_wifi, 10, LV_PART_MAIN);

    lv_obj_t* btn_label = lv_label_create(btn_wifi);
    lv_label_set_text(btn_label, LV_SYMBOL_WIFI "  WiFi Setup");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(btn_label);

    // Footer
    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "M5Stack Tab5 • ESP32-P4 • LVGL 9.2");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -30);
}
