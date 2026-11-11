/*
 * Host stand-in for task_report.cpp. Stall reports need the FreeRTOS run-time
 * stats and task stacks, which the host build does not have.
 */

#include "task_report.h"

void task_report_register_lock(const char*, SemaphoreHandle_t) {}

void task_report(const char*, const char*, int64_t, TaskHandle_t, const char*) {}
