/*
 * Arctic Heat Pump Controller
 * Stall detector for the LVGL (UI) thread.
 *
 * Every screen update, LVGL timer and /api/test UI call runs on the one LVGL
 * task or needs its lock, so when that thread stalls the screen freezes and
 * UI tests time out without saying why. An LVGL timer stamps a heartbeat; if
 * it stops for a couple of seconds the log gets a task snapshot, the holders
 * of the shared locks, and the code addresses on the UI thread's stack (see
 * task_report.h).
 */
#pragma once

/** @brief Start watching. Call once, from the LVGL thread or with its lock held. */
void ui_watch_start(void);
