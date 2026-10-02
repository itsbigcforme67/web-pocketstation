// FreeRTOS stand-in for desktop tests: tasks are real threads, so the locking is exercised for real.
#pragma once
#include "fake.h"
typedef std::timed_mutex *SemaphoreHandle_t;
typedef std::mutex portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex(); }
inline int xSemaphoreTake(SemaphoreHandle_t m, uint32_t ticks) { if (ticks == portMAX_DELAY) { m->lock(); return 1; } for (uint32_t i = 0; i <= ticks; i++) { if (m->try_lock()) return 1; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } return 0; }
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }
inline void portENTER_CRITICAL(portMUX_TYPE *m) { m->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE *m) { m->unlock(); }
inline void vTaskDelay(uint32_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
inline void vTaskDelete(void *) {}
inline uint32_t uxTaskGetStackHighWaterMark(void *) { return 4096; }
extern std::atomic<int> tasksStarted;
inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, uint32_t, void *arg, int, void *, int) { tasksStarted++; std::thread(fn, arg).detach(); return 1; }
