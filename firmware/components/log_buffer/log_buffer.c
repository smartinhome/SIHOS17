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
// Licznik linii logow zgubionych, gdy mutex byl zajety. Poprzednio
// xSemaphoreTake(mutex, 0) po prostu drop-owalo linie bez sladu -
// pod obciazeniem (dump HTTP + strumien ramek) w buforze wygladalo to
// jak "cisza w logach", co utrudnialo diagnoze.
static volatile uint32_t s_dropped = 0;
#include <time.h>

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
    // Bufor STATYCZNY (nie na stosie!) - 768B na stosie wywalało male taski ESP-IDF.
    // Chroniony tym samym mutexem co zapis do bufora kolowego.
    static char line[768];
    int n = 0;
    // Timeout 1 tick (~10 ms) zamiast 0: pozwol krotko poczekac na mutex zamiast
    // od razu drop-owac linii. Pod normalnym obciazeniem dump HTTP trzyma mutex
    // milisekundy, wiec 1 tick prawie zawsze wystarczy.
    if (s_mutex && xSemaphoreTake(s_mutex, 1) == pdTRUE) {
        // Jesli w miedzyczasie zgubily sie linie, dopisz o tym marker do bufora
        // ZANIM zapiszemy biezaca linie - dzieki temu widac gdzie byla luka.
        if (s_dropped > 0) {
            char drop_msg[64];
            int dn = snprintf(drop_msg, sizeof(drop_msg),
                              "[LOG: dropped %u lines]\n", (unsigned)s_dropped);
            if (dn > 0) append_to_buf(drop_msg, (size_t)dn);
            s_dropped = 0;
        }
        va_list args_copy;
        va_copy(args_copy, args);
        n = vsnprintf(line, sizeof(line), fmt, args_copy);
        va_end(args_copy);
        if (n > 0) {
            size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
            // Prefix [HH:MM:SS] gdy czas zsynchronizowany (SNTP, rok>2020)
            time_t now = time(NULL);
            struct tm ti;
            localtime_r(&now, &ti);
            if (ti.tm_year > 120) {   // rok > 2020 = czas ustawiony
                char ts[12];
                int tn = snprintf(ts, sizeof(ts), "[%02d:%02d:%02d] ",
                                  ti.tm_hour, ti.tm_min, ti.tm_sec);
                append_to_buf(ts, tn);
            }
            append_to_buf(line, len);
        }
        xSemaphoreGive(s_mutex);
    } else if (s_mutex) {
        // Mutex zajety dluzej niz tick - odnotuj drop dla nastepnego udanego wpisu.
        s_dropped++;
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
    if (!s_mutex || !out) return 0;
    // Guard: out_max < 2 znaczy nawet terminatora nie zmiescimy. Bez tego linie
    // ponizej robily `out_max - 1` (size_t) → underflow do SIZE_MAX → memcpy setek MB.
    if (out_max < 2) { if (out_max) out[0] = 0; return 0; }
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
