/*
 * Arctic Heat Pump Controller
 * Task snapshot for stall detectors (httpd_watch, ui_watch).
 *
 * When a watched thread stops making progress, task_report() logs what every
 * task was doing over the next second (state and CPU share, busiest first),
 * which task holds each registered lock, and, if the watched thread is
 * waiting rather than running, the code addresses on its stack. Decode those
 * with the matching ELF:
 *   riscv32-esp-elf-addr2line -pfiaC -e arctic_controller.elf 0x4800.. 0x4800..
 */
#pragma once

#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * @brief Name a mutex so reports say which task holds it. Register the locks a
 *        watched thread can wait on. Up to 12; later ones are ignored.
 */
void task_report_register_lock(const char* name, SemaphoreHandle_t lock);

/**
 * @brief Log "<what> stuck for N ms" and the snapshot described above. Blocks
 *        the caller for about a second while CPU use is sampled.
 *
 * @param tag      Log tag.
 * @param what     Thread description, e.g. "Web server thread".
 * @param stuck_us How long it has been stuck so far.
 * @param watched  The stuck task (may be null if not known yet).
 * @param mark     Suffix for the watched task's row, e.g. "<- web server".
 */
void task_report(const char* tag, const char* what, int64_t stuck_us,
                 TaskHandle_t watched, const char* mark);
