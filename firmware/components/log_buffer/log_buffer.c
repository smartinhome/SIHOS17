#include "log_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_BUF_SIZE 8192   // 8 KB bufor kolowy

static char              s_buf[LOG_BUF_SIZE];
static size_t            s_head = 0;     // pozycja zapisu
static bool              s_wrapped = false;
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t    s_orig_vprintf = NULL;

// Dopisz tekst do bufora kolowego
static void append_to_buf(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = data[i];
        if (s_head >= LOG_BUF_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
}

// Nasza funkcja przechwytujaca — wola oryginalna (UART) i zapisuje do bufora
static int log_vprintf(const char *fmt, va_list args) {
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (n > 0) {
        size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        if (s_mutex && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
            append_to_buf(line, len);
            xSemaphoreGive(s_mutex);
        }
    }

    // Przekaz dalej do oryginalnego (UART)
    if (s_orig_vprintf)
        return s_orig_vprintf(fmt, args);
    return n;
}

void log_buffer_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_head = 0;
    s_wrapped = false;
    memset(s_buf, 0, sizeof(s_buf));
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}

size_t log_buffer_dump(char *out, size_t out_max) {
    if (!s_mutex) { out[0] = 0; return 0; }
    size_t written = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_wrapped) {
        // Najpierw od head do konca (najstarsze)
        size_t tail_len = LOG_BUF_SIZE - s_head;
        size_t copy = tail_len < out_max - 1 ? tail_len : out_max - 1;
        memcpy(out, s_buf + s_head, copy);
        written = copy;
        // Potem od 0 do head (nowsze)
        if (written < out_max - 1) {
            size_t copy2 = s_head < (out_max - 1 - written) ? s_head : (out_max - 1 - written);
            memcpy(out + written, s_buf, copy2);
            written += copy2;
        }
    } else {
        size_t copy = s_head < out_max - 1 ? s_head : out_max - 1;
        memcpy(out, s_buf, copy);
        written = copy;
    }
    out[written] = 0;
    xSemaphoreGive(s_mutex);
    return written;
}

void log_buffer_clear(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = 0;
    s_wrapped = false;
    xSemaphoreGive(s_mutex);
}
