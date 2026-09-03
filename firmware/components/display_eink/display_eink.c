#include "display_eink.h"
#include "qr_data.h"
#include "logo_data.h"
#include "font_data.h"
#include "history.h"
#include "wmbus_decoder.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "eink";

// --- Wymiary panelu (fizyczne, kontroler SSD1680) ---
// Panel 2.13" v3: 122 x 250 (szer x wys w pamieci kontrolera).
#define PANEL_W   122          // szerokosc fizyczna (pamiec: 16 bajtow/wiersz)
#define PANEL_H   250          // wysokosc fizyczna
#define PANEL_BPR ((PANEL_W + 7) / 8)   // 16 bajtow na wiersz
#define FB_SIZE   (PANEL_BPR * PANEL_H) // 16 * 250 = 4000 B

// --- Wymiary logiczne (po rotacji 270, jak w ESPHome) ---
#define LCD_W  250
#define LCD_H  122

static spi_device_handle_t s_spi = NULL;
static display_eink_config_t s_cfg;
static uint8_t s_fb[FB_SIZE];   // framebuffer, 1 = czarny piksel? (SSD1680: 0=czarny,1=bialy w RAM)

// Mutex serializujacy WSZYSTKIE operacje e-ink (rysowanie po s_fb + odswiezanie).
// button_task i refresh_task wolaja te same funkcje na tym samym uchwycie SPI,
// a spi_master wymaga uzywania urzadzenia z jednego taska naraz - w szczegolnosci
// rownolegle spi_device_acquire_bus() z dwoch taskow psulo blokade magistrali
// i zawieszalo na stale cala SPI2 (w tym odbior CC1101).
static SemaphoreHandle_t s_eink_mutex = NULL;
static inline void eink_lock(void)   { if (s_eink_mutex) xSemaphoreTake(s_eink_mutex, portMAX_DELAY); }
static inline void eink_unlock(void) { if (s_eink_mutex) xSemaphoreGive(s_eink_mutex); }

// ---------- niski poziom SPI / GPIO ----------

static void eink_wait_busy(void) {
    // BUSY = high oznacza zajety (SSD1680)
    int guard = 0;
    while (gpio_get_level(s_cfg.pin_busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++guard > 500) { // 5 s timeout
            ESP_LOGW(TAG, "BUSY timeout");
            break;
        }
    }
}

static void eink_cmd(uint8_t c) {
    gpio_set_level(s_cfg.pin_dc, 0); // command
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &c;
    spi_device_transmit(s_spi, &t);
}

static void eink_data(uint8_t d) {
    gpio_set_level(s_cfg.pin_dc, 1); // data
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &d;
    spi_device_transmit(s_spi, &t);
}

static void eink_data_buf(const uint8_t *buf, int len) {
    gpio_set_level(s_cfg.pin_dc, 1);
    // Magistrala ma max_transfer_sz=512 (ustawione przez CC1101). Wysylamy w porcjach.
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > 256) chunk = 256;
        spi_transaction_t t = {0};
        t.length = 8 * chunk;
        t.tx_buffer = buf + off;
        spi_device_transmit(s_spi, &t);
        off += chunk;
    }
}

static void eink_reset(void) {
    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ---------- sekwencja init SSD1680 ----------

// Tablice waveform (LUT) i komendy - dokladnie jak sterownik ESPHome 2.13 v3.
// Pierwszy bajt 0x32 to komenda Write LUT.
static const uint8_t FULL_LUT[] = {
    0x32,
    0x80,0x4A,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x4A,0x80,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x80,0x4A,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x4A,0x80,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x0F,0x00,0x00,0x02,0x0F,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
};

// LUT czesciowego odswiezania (partial) - bez migania/inwersji calego ekranu.
static const uint8_t PARTIAL_LUT[] = {
    0x32,
    0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
};

// Pelne odswiezenie: przy pierwszym odswiezeniu po starcie (restart programowy lub
// sprzetowy) oraz raz na dobe o godzinie 3:00. Pomiedzy - tylko czesciowe.
static bool s_did_first_refresh = false;   // false do pierwszego odswiezenia po starcie
static int  s_last_full_yday = -1;         // dzien roku ostatniego pelnego o 3:00 (-1 = brak)

static void eink_panel_init(void) {
    // Podwojny reset jak ESPHome (reset_ + send_reset_)
    eink_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    eink_wait_busy();

    eink_cmd(0x12); // SW_RESET
    eink_wait_busy();

    // DRV_OUT_CTL {0x01,0x27,0x01,0x00} - driver output control (250-1=0x0127)
    eink_cmd(0x01); eink_data(0x27); eink_data(0x01); eink_data(0x00);
    // DATA_ENTRY {0x11,0x03}
    eink_cmd(0x11); eink_data(0x03);
    // CMD5 {0x37, 0,0,0,0,0,0x40,0,0,0,0}
    eink_cmd(0x37);
    eink_data(0x00); eink_data(0x00); eink_data(0x00); eink_data(0x00); eink_data(0x00);
    eink_data(0x40); eink_data(0x00); eink_data(0x00); eink_data(0x00); eink_data(0x00);

    // set_window (0..height): RAM_X_START {0x44,0x00,121/8=15}, RAM_Y_START {0x45,0,0,249,0}
    eink_cmd(0x44); eink_data(0x00); eink_data(121 / 8);
    eink_cmd(0x45); eink_data(0x00); eink_data(0x00); eink_data((PANEL_H - 1) & 0xFF); eink_data(0x00);
    eink_cmd(0x4E); eink_data(0x00);                       // RAM X counter
    eink_cmd(0x4F); eink_data(0x00); eink_data(0x00);      // RAM Y counter

    // BORDER_FULL {0x3C,0x05}
    eink_cmd(0x3C); eink_data(0x05);
    // DISPLAY_UPDATE {0x21,0x00,0x80}
    eink_cmd(0x21); eink_data(0x00); eink_data(0x80);
    // TEMP_SENS {0x18,0x80}
    eink_cmd(0x18); eink_data(0x80);
    eink_wait_busy();

    // write_lut(FULL_LUT) + CMD1/GATEV/SRCV/VCOM
    eink_cmd(FULL_LUT[0]);
    eink_data_buf(FULL_LUT + 1, sizeof(FULL_LUT) - 1);
    eink_cmd(0x3F); eink_data(0x22);                       // CMD1
    eink_cmd(0x03); eink_data(0x17);                       // GATEV
    eink_cmd(0x04); eink_data(0x41); eink_data(0x0C); eink_data(0x32); // SRCV
    eink_cmd(0x2C); eink_data(0x36);                       // VCOM
    eink_wait_busy();
}

static void eink_set_cursor(void) {
    eink_cmd(0x4E); // RAM X address counter
    eink_data(0x00);
    eink_cmd(0x4F); // RAM Y address counter
    eink_data(0x00);
    eink_data(0x00);
}

static void eink_full_refresh(void) {
    // Wylaczny dostep do magistrali SPI na czas odswiezania (wspoldzielone z CC1101).
    spi_device_acquire_bus(s_spi, portMAX_DELAY);

    eink_wait_busy();
    eink_set_cursor();

    // Zaladuj LUT pelny przed kazdym odswiezeniem (jak full_update_ w ESPHome).
    eink_cmd(FULL_LUT[0]);
    eink_data_buf(FULL_LUT + 1, sizeof(FULL_LUT) - 1);
    eink_cmd(0x3F); eink_data(0x22);
    eink_cmd(0x03); eink_data(0x17);
    eink_cmd(0x04); eink_data(0x41); eink_data(0x0C); eink_data(0x32);
    eink_cmd(0x2C); eink_data(0x36);

    // Pisz OBA bufory: WRITE_BUFFER (0x24) i WRITE_BASE (0x26).
    eink_set_cursor();
    eink_cmd(0x24);
    eink_data_buf(s_fb, FB_SIZE);
    eink_set_cursor();
    eink_cmd(0x26);
    eink_data_buf(s_fb, FB_SIZE);

    // ON_FULL {0x22,0xC7} + ACTIVATE.
    eink_cmd(0x22); eink_data(0xC7);
    eink_cmd(0x20); // ACTIVATE
    eink_wait_busy();

    spi_device_release_bus(s_spi);
}

// Czesciowe odswiezanie - szybkie, bez migania. Wymaga wczesniejszego full
// (ktory ustawil base buffer 0x26 jako referencje).
static void eink_partial_refresh(void) {
    spi_device_acquire_bus(s_spi, portMAX_DELAY);

    // Krotki reset (jak send_reset_ w ESPHome partial_update_).
    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    eink_wait_busy();

    // Po resecie RST przywroc ustawienia obszaru RAM (data entry + window).
    eink_cmd(0x11); eink_data(0x03);                       // data entry mode
    eink_cmd(0x44); eink_data(0x00); eink_data(121 / 8);   // RAM X start/end
    eink_cmd(0x45); eink_data(0x00); eink_data(0x00); eink_data((PANEL_H - 1) & 0xFF); eink_data(0x00);

    // LUT czesciowy + komendy.
    eink_cmd(PARTIAL_LUT[0]);
    eink_data_buf(PARTIAL_LUT + 1, sizeof(PARTIAL_LUT) - 1);
    eink_cmd(0x3F); eink_data(0x22);
    eink_cmd(0x03); eink_data(0x17);
    eink_cmd(0x04); eink_data(0x41); eink_data(0x0C); eink_data(0x32);
    eink_cmd(0x2C); eink_data(0x36);

    // BORDER_PART {0x3C,0x80} + UPSEQ {0x22,0xC0} + ACTIVATE.
    eink_cmd(0x3C); eink_data(0x80);
    eink_cmd(0x22); eink_data(0xC0);
    eink_cmd(0x20);
    eink_wait_busy();

    // Pisz tylko bufor obrazu (0x24) - panel odswiezy roznice wzgledem base.
    eink_set_cursor();
    eink_cmd(0x24);
    eink_data_buf(s_fb, FB_SIZE);

    // ON_PARTIAL {0x22,0x0F} + ACTIVATE.
    eink_cmd(0x22); eink_data(0x0F);
    eink_cmd(0x20);
    eink_wait_busy();

    spi_device_release_bus(s_spi);
}

// Dyspozytor: pelne odswiezenie (ustawia base, czysci artefakty) przy pierwszym
// odswiezeniu po starcie oraz raz na dobe o godzinie 3:00; pozostale czesciowe.
static void eink_refresh(void) {
    bool do_full = false;

    if (!s_did_first_refresh) {
        do_full = true;                 // pierwsze po restarcie (programowym/sprzetowym)
        s_did_first_refresh = true;
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        // O 3:00 (godzina 3, dowolna minuta tego cyklu) raz na dobe.
        if (tm.tm_hour == 3 && s_last_full_yday != tm.tm_yday) {
            do_full = true;
            s_last_full_yday = tm.tm_yday;
        }
    }

    if (do_full) {
        eink_full_refresh();
    } else {
        eink_partial_refresh();
    }
}


// ---------- framebuffer / rysowanie ----------
// Konwencja: w SSD1680 bit=1 -> bialy, bit=0 -> czarny.
// My czyscimy do bieli (0xFF) i ustawiamy czarne piksele (bit=0).
// Uklad logiczny 250x122 z rotacja 270 mapujemy na pamiec 122x250.

static void fb_clear_white(void) {
    memset(s_fb, 0xFF, FB_SIZE);
}

// Ustaw piksel w ukladzie LOGICZNYM (0..249, 0..121). 1=czarny.
static void fb_set_pixel(int lx, int ly, int black) {
    if (lx < 0 || lx >= LCD_W || ly < 0 || ly >= LCD_H) return;
    // Wyswietlacz montowany "do gory nogami" - obrot panelu o 180 stopni
    // wzgledem dotychczasowej rotacji 270 (efektywnie rotacja 90).
    // Bazowe ESPHome rotation 270: px = ly, py = (LCD_W-1) - lx;
    // po odwroceniu obu osi fizycznych: px = (PANEL_W-1) - ly, py = lx.
    int px = (PANEL_W - 1) - ly;
    int py = lx;
    if (px < 0 || px >= PANEL_W || py < 0 || py >= PANEL_H) return;
    int idx = py * PANEL_BPR + (px >> 3);
    uint8_t mask = 0x80 >> (px & 7);
    if (black) s_fb[idx] &= ~mask; // czarny = bit 0
    else       s_fb[idx] |= mask;  // bialy  = bit 1
}

static void fb_fill_rect(int x, int y, int w, int h, int black) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_set_pixel(x + i, y + j, black);
}

// Rysuj bitmape (pakowana MSB first, 1=czarny punkt) ze skalowaniem.
static void fb_draw_bitmap(int x0, int y0, const uint8_t *data, int size, int bpr, int scale) {
    for (int ry = 0; ry < size; ry++) {
        for (int rx = 0; rx < size; rx++) {
            uint8_t byte = data[ry * bpr + (rx >> 3)];
            int bit = (byte >> (7 - (rx & 7))) & 1;
            if (bit) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_set_pixel(x0 + rx * scale + sx, y0 + ry * scale + sy, 1);
            }
        }
    }
}

// ---------- tekst (fonty Terminus z font_data.h) ----------

static const glyph_t *font_find(const font_t *f, uint32_t cp) {
    for (int i = 0; i < f->n_glyphs; i++)
        if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return NULL;
}

// Dekoduj jeden znak UTF-8, zwraca liczbe bajtow.
static int utf8_next(const char *s, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c >> 5) == 0x6)  { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c >> 4) == 0xE)  { *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c >> 3) == 0x1E) { *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *cp = '?'; return 1;
}

// Szerokosc napisu w pikselach (do wysrodkowania).
static int fb_text_width(const font_t *f, const char *txt) {
    int w = 0; uint32_t cp; int i = 0;
    while (txt[i]) {
        int n = utf8_next(txt + i, &cp); i += n;
        const glyph_t *g = font_find(f, cp);
        w += g ? g->adv : f->cell_w;
    }
    return w;
}

// Rysuj napis. (x,y) = lewy gorny rog komorki tekstu. 1=czarny.
static void fb_draw_text(const font_t *f, int x, int y, const char *txt) {
    uint32_t cp; int i = 0; int penx = x;
    while (txt[i]) {
        int n = utf8_next(txt + i, &cp); i += n;
        const glyph_t *g = font_find(f, cp);
        if (!g) { penx += f->cell_w; continue; }
        int bpr = (g->w + 7) / 8;
        for (int ry = 0; ry < g->h; ry++) {
            for (int rx = 0; rx < g->w; rx++) {
                uint8_t byte = f->bitmap[g->off + ry * bpr + (rx >> 3)];
                int bit = (byte >> (7 - (rx & 7))) & 1;
                if (bit) {
                    int gx = penx + g->xoff + rx;
                    int gy = y + f->ascent - (g->h + g->yoff) + ry;
                    fb_set_pixel(gx, gy, 1);
                }
            }
        }
        penx += g->adv;
    }
}

// Rysuj napis wysrodkowany poziomo wzgledem (cx).
static void fb_draw_text_center(const font_t *f, int cx, int y, const char *txt) {
    int w = fb_text_width(f, txt);
    fb_draw_text(f, cx - w / 2, y, txt);
}

// Ten sam rysunek, ale kazdy piksel fontu powielony scale x scale razy.
// Uzywane przez strone zegara: F32 w skali 2 daje cyfre o 40 px tuszu.
static void fb_draw_text_scaled(const font_t *f, int x, int y, const char *txt, int scale) {
    if (scale < 1) scale = 1;
    uint32_t cp; int i = 0; int penx = x;
    while (txt[i]) {
        int n = utf8_next(txt + i, &cp); i += n;
        const glyph_t *g = font_find(f, cp);
        if (!g) { penx += f->cell_w * scale; continue; }
        int bpr = (g->w + 7) / 8;
        for (int ry = 0; ry < g->h; ry++) {
            for (int rx = 0; rx < g->w; rx++) {
                uint8_t byte = f->bitmap[g->off + ry * bpr + (rx >> 3)];
                if (!((byte >> (7 - (rx & 7))) & 1)) continue;
                int gx = penx + (g->xoff + rx) * scale;
                int gy = y + (f->ascent - (g->h + g->yoff) + ry) * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_set_pixel(gx + sx, gy + sy, 1);
            }
        }
        penx += g->adv * scale;
    }
}

static void fb_draw_text_center_scaled(const font_t *f, int cx, int y, const char *txt, int scale) {
    fb_draw_text_scaled(f, cx - (fb_text_width(f, txt) * scale) / 2, y, txt, scale);
}

// Kontur prostokata (1px).
static void fb_rect(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) { fb_set_pixel(x + i, y, 1); fb_set_pixel(x + i, y + h - 1, 1); }
    for (int j = 0; j < h; j++) { fb_set_pixel(x, y + j, 1); fb_set_pixel(x + w - 1, y + j, 1); }
}

// Tekst odwrocony: bialy na czarnym tle (dla naglowka). 0 = bialy piksel.
static void fb_draw_text_inv(const font_t *f, int x, int y, const char *txt) {
    uint32_t cp; int i = 0; int penx = x;
    while (txt[i]) {
        int n = utf8_next(txt + i, &cp); i += n;
        const glyph_t *g = font_find(f, cp);
        if (!g) { penx += f->cell_w; continue; }
        int bpr = (g->w + 7) / 8;
        for (int ry = 0; ry < g->h; ry++) {
            for (int rx = 0; rx < g->w; rx++) {
                uint8_t byte = f->bitmap[g->off + ry * bpr + (rx >> 3)];
                int bit = (byte >> (7 - (rx & 7))) & 1;
                if (bit) {
                    int gx = penx + g->xoff + rx;
                    int gy = y + f->ascent - (g->h + g->yoff) + ry;
                    fb_set_pixel(gx, gy, 0); // bialy
                }
            }
        }
        penx += g->adv;
    }
}

// ---------- API ----------

bool display_eink_init(const display_eink_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(*cfg));
    if (!s_eink_mutex) s_eink_mutex = xSemaphoreCreateMutex();

    // Piny sterujace
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << cfg->pin_dc) | (1ULL << cfg->pin_rst),
    };
    gpio_config(&io);
    gpio_config_t in = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << cfg->pin_busy),
    };
    gpio_config(&in);

    // Dodaj wyswietlacz jako urzadzenie na ISTNIEJACEJ magistrali (CC1101 ja zainicjalizowal).
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = cfg->pin_cs,
        .queue_size = 4,
    };
    esp_err_t r = spi_bus_add_device(cfg->spi_host, &dev, &s_spi);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device blad: %d", r);
        return false;
    }

    eink_panel_init();
    ESP_LOGI(TAG, "Wyswietlacz e-ink zainicjalizowany (SSD1680 2.13\" v3)");
    return true;
}

void display_eink_show_splash(void) {
    eink_lock();
    fb_clear_white();

    // Lewa strefa: 0..qr_x, prawa: QR. Napis www.smartinhome.pl ma ~144px
    // w foncie F14 - wysrodkowany wzgledem srodka strefy lewej (nie srodka logo).
    int qr_scale = 3;
    int qr_px = QR_SIZE * qr_scale;            // 87
    int qr_x = LCD_W - qr_px - 10;             // ~153
    int qr_y = (LCD_H - qr_px) / 2;            // ~17
    int left_w = qr_x - 6;                     // szerokosc strefy lewej (~147)
    int left_cx = left_w / 2;                  // srodek strefy lewej

    // Logo wysrodkowane poziomo w strefie lewej, u gory.
    int logo_x = left_cx - LOGO_SIZE / 2;
    if (logo_x < 2) logo_x = 2;
    int logo_y = 0;
    fb_draw_bitmap(logo_x, logo_y, &LOGO_DATA[0][0], LOGO_SIZE, LOGO_BYTES_PER_ROW, 1);

    // Napis pod logo (realna tresc logo siega ~y=98), wysrodkowany w strefie lewej.
    int txt_y = 100;
    fb_draw_text_center(&F14, left_cx, txt_y, "www.smartinhome.pl");

    // QR po prawej, biala quiet-zone.
    fb_fill_rect(qr_x - 4, qr_y - 4, qr_px + 8, qr_px + 8, 0);
    fb_draw_bitmap(qr_x, qr_y, &QR_DATA[0][0], QR_SIZE, QR_BYTES_PER_ROW, qr_scale);

    eink_refresh();
    eink_unlock();
    ESP_LOGI(TAG, "Splash (logo + napis + QR) wyswietlony");
}

// ---------- strony licznikow (dane z historii) ----------

#define MAX_PAGES 12
static char s_page_ids[MAX_PAGES][12];  // ID licznikow ze sledzonymi polami
static int  s_page_count = 0;
static int  s_cur_page = 0;

// Etykieta i jednostka wg rodzaju licznika (kind: 1=woda,2=prad,3=gaz).
static const char* kind_title(int kind) {
    switch (kind) { case 1: return "Woda"; case 2: return "Elektryczność"; case 3: return "Gaz"; default: return "Licznik"; }
}
static const char* kind_unit(int kind) {
    switch (kind) { case 1: return "m\u00b3"; case 2: return "kWh"; case 3: return "m\u00b3"; default: return ""; }
}

// Sformatuj liczbe z jednostka do bufora.
static void fmt_val(char *buf, int cap, double v, const char *unit, int decimals) {
    if (decimals <= 0) snprintf(buf, cap, "%.0f %s", v, unit);
    else if (decimals == 2) snprintf(buf, cap, "%.2f %s", v, unit);
    else snprintf(buf, cap, "%.3f %s", v, unit);
}

// Ladniejsza nazwa pola do wyswietlenia.
static const char* field_label(const char *field) {
    if (strstr(field, "energia")) return "energia";
    if (strstr(field, "produkcja")) return "produkcja";
    if (strstr(field, "total_m3")) return "zużycie";
    if (strstr(field, "moc_produkcji")) return "moc prod.";
    if (strstr(field, "moc")) return "moc";
    if (strstr(field, "napiecie_l1")) return "napięcie L1";
    if (strstr(field, "napiecie_l2")) return "napięcie L2";
    if (strstr(field, "napiecie_l3")) return "napięcie L3";
    return field;
}

// Jednostka pola z nazwy.
static const char* field_unit(const char *field) {
    if (strstr(field, "kwh")) return "kWh";
    if (strstr(field, "kw"))  return "kW";
    if (strstr(field, "_v"))  return "V";
    if (strstr(field, "m3"))  return "m\u00b3";
    return "";
}

// Wyodrebnij pole z klucza "id:pole" (lub pusty gdy sam id).
static const char* key_field(const char *key) {
    const char *c = strchr(key, ':');
    return c ? c + 1 : "";
}

// Narysuj jedna strone licznika (wszystkie sledzone pola tego ID).
static void draw_meter_page(const char *id, int page_no, int total_pages) {
    fb_clear_white();

    // Zbierz wszystkie sledzone klucze tego licznika.
    char keys[8][40];
    int nkeys = history_keys_for_id(id, keys, 8);

    // Wybierz glowne pole kumulacyjne (energia/woda/gaz) - do duzego widoku.
    int main_idx = -1;
    hist_display_t main_s; bool main_ok = false;
    for (int i = 0; i < nkeys; i++) {
        hist_display_t s;
        if (history_display_summary(keys[i], &s) && s.cumulative) {
            main_idx = i; main_s = s; main_ok = true; break;
        }
    }
    // Tytul i numer strony. Wlasna nazwa uzytkownika ma pierwszenstwo przed
    // tytulem typu (Elektrycznosc/Woda/Gaz); gdy brak nazwy - tytul typu.
    int kind = main_ok ? main_s.kind : 0;
    fb_fill_rect(0, 0, LCD_W, 16, 1);
    const char *cust = nvs_config_meter_name(id);
    const char *title = (cust && cust[0]) ? cust : (kind ? kind_title(kind) : "Licznik");
    fb_draw_text_inv(&F14, 3, 0, title);
    char pg[12];
    snprintf(pg, sizeof(pg), "%d/%d", page_no, total_pages);
    int pgw = fb_text_width(&F14, pg);
    fb_draw_text_inv(&F14, LCD_W - pgw - 4, 0, pg);

    if (nkeys == 0) {
        fb_draw_text(&F14, 6, 50, "Brak śledzonych pól");
        char idline[40];
        snprintf(idline, sizeof(idline), "ID: %s", id);
        fb_draw_text(&F14, 6, 70, idline);
        return;
    }

    uint32_t last_ts = 0;

    if (main_ok && main_s.has_value) {
        const char *unit = field_unit(key_field(keys[main_idx]));
        if (unit[0] == 0) unit = kind_unit(kind);
        last_ts = main_s.last_ts;

        // ZUZYCIE DZIS duza czcionka.
        char big[24];
        if (main_s.has_today) fmt_val(big, sizeof(big), main_s.today, unit, 3);
        else snprintf(big, sizeof(big), "--.- %s", unit);
        fb_draw_text(&F24, 3, 18, big);
        fb_draw_text(&F14, 3, 44, "zużycie dziś");

        // Dla wody: zuzycie DZIS w litrach w prawym gornym rogu (analogicznie do
        // aktualnego poboru W przy energii). m3 zostaja w glownym widoku, litry to
        // dodatkowa informacja (czytelniejsza dla malych dziennych wartosci).
        if (kind == 1 && main_s.has_today) {
            char lt[20];
            snprintf(lt, sizeof(lt), "%.0f L", main_s.today * 1000.0);
            int lt_w = fb_text_width(&F24, lt);
            int lt_x = LCD_W - lt_w - 4;
            fb_fill_rect(lt_x - 4, 17, LCD_W - lt_x + 4, 26, 1);
            fb_draw_text_inv(&F24, lt_x, 18, lt);
            int lbl_w = fb_text_width(&F14, "dziś L");
            fb_draw_text(&F14, LCD_W - lbl_w - 4, 44, "dziś L");
        }

        // POLA CHWILOWE (moc, napiecia) - jesli sledzone, w prawej czesci ekranu.
        // Moc -> prawy gorny rog w W (jak ESPHome akt. pobor). Napiecia -> lista.
        int volt_y = 60;
        for (int i = 0; i < nkeys; i++) {
            if (i == main_idx) continue;
            const char *f = key_field(keys[i]);
            hist_display_t fs;
            if (!history_display_summary(keys[i], &fs) || !fs.has_value) continue;
            // UWAGA: dopasowanie DOKLADNE. Luzne strstr(f,"moc") lapalo tez
            // moc_bierna_l_var / moc_bierna_c_var / moc_max_kw i pokazywalo je
            // jako "akt. pobor" przemnozone przez 1000 (bledna wartosc w W).
            if (strcmp(f, "moc_kw") == 0) {
                // Aktualny pobor w W (kW * 1000), prawy gorny rog, odwrocony.
                char pw[20];
                snprintf(pw, sizeof(pw), "%.0f W", fs.last_total * 1000.0);
                int pw_w = fb_text_width(&F24, pw);
                int pw_x = LCD_W - pw_w - 4;
                fb_fill_rect(pw_x - 4, 17, LCD_W - pw_x + 4, 26, 1);
                fb_draw_text_inv(&F24, pw_x, 18, pw);
                int lbl_w = fb_text_width(&F14, "akt. pobór");
                fb_draw_text(&F14, LCD_W - lbl_w - 4, 44, "akt. pobór");
            } else if (strstr(f, "bierna")) {
                // Moc bierna w VAR (NIE mnozyc przez 1000 - juz jest w VAR).
                char v[16];
                snprintf(v, sizeof(v), "%.0f", fs.last_total);
                const char *lbl = strstr(f, "_c_") ? "Qc:" : "Ql:";
                if (volt_y <= 90) {
                    fb_draw_text(&F14, 188, volt_y, lbl);
                    fb_draw_text(&F14, 210, volt_y, v);
                    volt_y += 14;
                }
            } else if (strncmp(f, "prad", 4) == 0) {
                // Prad fazowy w amperach.
                char v[16];
                snprintf(v, sizeof(v), "%.1fA", fs.last_total);
                const char *lbl = strstr(f, "l1") ? "I1:" : strstr(f, "l2") ? "I2:" :
                                  strstr(f, "l3") ? "I3:" : "I:";
                if (volt_y <= 90) {
                    fb_draw_text(&F14, 188, volt_y, lbl);
                    fb_draw_text(&F14, 210, volt_y, v);
                    volt_y += 14;
                }
            } else if (strncmp(f, "napiecie", 8) == 0) {
                // Napiecie - w prawej czesci ramki statystyk.
                char v[16];
                snprintf(v, sizeof(v), "%.0fV", fs.last_total);
                const char *lbl = strstr(f, "l1") ? "L1:" : strstr(f, "l2") ? "L2:" : strstr(f, "l3") ? "L3:" : "U:";
                if (volt_y <= 90) {
                    fb_draw_text(&F14, 188, volt_y, lbl);
                    fb_draw_text(&F14, 210, volt_y, v);
                    volt_y += 14;
                }
            }
        }

        // RAMKA statystyki (wczoraj/przedwczoraj/licznik).
        fb_rect(2, 58, LCD_W - 4, 48);
        char line[32];
        if (main_s.has_yesterday) {
            fmt_val(line, sizeof(line), main_s.yesterday, unit, 3);
            fb_draw_text(&F14, 7, 60, "wczoraj:");
            fb_draw_text(&F14, 80, 60, line);
        } else fb_draw_text(&F14, 7, 60, "wczoraj:   --");
        if (main_s.has_day_before) {
            fmt_val(line, sizeof(line), main_s.day_before, unit, 3);
            fb_draw_text(&F14, 7, 74, "przedwcz:");
            fb_draw_text(&F14, 80, 74, line);
        } else fb_draw_text(&F14, 7, 74, "przedwcz:  --");
        fmt_val(line, sizeof(line), main_s.last_total, unit, 3);
        fb_draw_text(&F14, 7, 90, "licznik:");
        fb_draw_text(&F14, 80, 90, line);
    } else {
        // Brak pola kumulacyjnego - pokaz tylko pola chwilowe (np. same napiecia).
        int y = 22;
        for (int i = 0; i < nkeys && y < 104; i++) {
            hist_display_t s;
            if (!history_display_summary(keys[i], &s) || !s.has_value) continue;
            const char *f = key_field(keys[i]);
            const char *unit = field_unit(f);
            char line[40];
            snprintf(line, sizeof(line), "%s:", field_label(f));
            fb_draw_text(&F14, 6, y, line);
            char val[24];
            fmt_val(val, sizeof(val), s.last_total, unit, s.cumulative ? 3 : 0);
            fb_draw_text(&F14, 130, y, val);
            if (s.last_ts > last_ts) last_ts = s.last_ts;
            y += 16;
        }
    }

    // ODCZYT.
    if (last_ts) {
        time_t tt = last_ts; struct tm tm; localtime_r(&tt, &tm);
        char ts[40];
        strftime(ts, sizeof(ts), "%d.%m.%Y  %H:%M:%S", &tm);
        char odczyt[52];
        snprintf(odczyt, sizeof(odczyt), "odczyt: %s", ts);
        fb_draw_text(&F14, 3, 108, odczyt);
    } else {
        fb_draw_text(&F14, 3, 108, "odczyt: oczekiwanie...");
    }
}

// Odswiez liste stron z historii (unikalne ID sledzonych licznikow).
// Ekran diagnostyczny - pokazuje czemu nie ma stron (do debugowania).
static void draw_diag(void) {
    fb_clear_white();
    fb_fill_rect(0, 0, LCD_W, 16, 1);
    fb_draw_text_inv(&F14, 3, 0, "Diagnostyka e-ink");

    char line[64];
    int nt = history_tracked_count();
    snprintf(line, sizeof(line), "Śledzonych pól: %d", nt);
    fb_draw_text(&F14, 4, 22, line);

    char first[28]; history_tracked_first(first, sizeof(first));
    if (first[0]) {
        snprintf(line, sizeof(line), "Pierwszy: %s", first);
        fb_draw_text(&F14, 4, 40, line);
    } else {
        fb_draw_text(&F14, 4, 40, "Brak - oznacz pole w UI");
        fb_draw_text(&F14, 4, 56, "(Liczniki -> + przy wartości)");
    }

    // Ile licznikow modul juz uslyszal w eterze (odswiezane co minute razem
    // z cala strona diagnostyczna).
    int det = wmbus_decoder_get_seen_count();
    char dl[40];
    if (det == 0) {
        snprintf(dl, sizeof(dl), "Nasluchuje - brak licznikow");
    } else if (det == 1) {
        snprintf(dl, sizeof(dl), "Wykryto 1 licznik");
    } else if (det % 10 >= 2 && det % 10 <= 4 && (det % 100 < 12 || det % 100 > 14)) {
        snprintf(dl, sizeof(dl), "Wykryto %d liczniki", det);   // 2-4, 22-24...
    } else {
        snprintf(dl, sizeof(dl), "Wykryto %d licznikow", det);  // 5+, 11-14...
    }
    fb_draw_text(&F14, 4, 74, dl);

    // Adres, pod ktorym dostepny jest panel - w trybie AP adres bramki
    // punktu dostepowego, po polaczeniu z siecia domowa adres z DHCP.
    char ip[16]; wifi_manager_get_ip(ip, sizeof(ip));
    char ipl[40];
    wifi_state_t wst = wifi_manager_get_state();
    if (wst == WIFI_STATE_AP_MODE) {
        snprintf(ipl, sizeof(ipl), "AP: %s", ip);
    } else if (wst == WIFI_STATE_CONNECTED && strcmp(ip, "0.0.0.0") != 0) {
        snprintf(ipl, sizeof(ipl), "IP: %s", ip);
    } else {
        snprintf(ipl, sizeof(ipl), "IP: brak polaczenia");
    }
    fb_draw_text(&F14, 4, 90, ipl);

    fb_draw_text(&F14, 4, 106, "www.smartinhome.pl");
    eink_refresh();
}

// ---------- strona zegara (ostatnia, za wszystkimi licznikami) ----------

// Miesiace w dopelniaczu ("1 wrzesnia 2026"). Wszystkie polskie znaki, ktorych
// tu uzywamy, sa w F16 (sprawdzone: s z kreska w "wrzesnia", z z kreska w
// "pazdziernika"). Dzien tygodnia celowo pomijamy - "poniedzialek, 15
// pazdziernika 2026" to 34 znaki x 8 px = 272 px, czyli wiecej niz 250 px
// szerokosci panelu. Sama data miesci sie zawsze (max 20 znakow = 160 px).
static const char *PL_MONTHS[12] = {
    "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
    "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
};

// Czas przed synchronizacja SNTP stoi w 1970. Ponizej tego progu (2020-09-13)
// pokazujemy komunikat zamiast falszywej daty.
#define TIME_SYNCED_MIN 1600000000

static void draw_clock_page(int page_no, int total_pages) {
    fb_clear_white();
    fb_fill_rect(0, 0, LCD_W, 16, 1);
    fb_draw_text_inv(&F14, 3, 0, "Czas");
    char pg[12];
    snprintf(pg, sizeof(pg), "%d/%d", page_no, total_pages);
    int pgw = fb_text_width(&F14, pg);
    fb_draw_text_inv(&F14, LCD_W - pgw - 4, 0, pg);

    time_t now = time(NULL);
    if (now < TIME_SYNCED_MIN) {
        fb_draw_text_center_scaled(&F32, LCD_W / 2, 19, "--:--", 2);
        fb_draw_text_center(&F16, LCD_W / 2, 91, "Czekam na synchronizację");
        return;
    }

    struct tm tm;
    localtime_r(&now, &tm);
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    fb_draw_text_center_scaled(&F32, LCD_W / 2, 19, hhmm, 2);

    char date[48];
    int mon = (tm.tm_mon >= 0 && tm.tm_mon < 12) ? tm.tm_mon : 0;
    snprintf(date, sizeof(date), "%d %s %d", tm.tm_mday, PL_MONTHS[mon], tm.tm_year + 1900);
    fb_draw_text_center(&F16, LCD_W / 2, 91, date);
}

static void rebuild_pages(void) {
    s_page_count = history_tracked_meter_ids(s_page_ids, MAX_PAGES);
    if (s_cur_page >= s_page_count + (nvs_config_eink_clock() ? 1 : 0)) s_cur_page = 0;
}

// Zegar jest doklejany jako strona o indeksie s_page_count, czyli zawsze
// ostatnia. Gdy nie ma zadnego sledzonego pola, zostaje jedyna - i wtedy
// zastepuje ekran diagnostyczny.
static bool is_clock_page(int idx) {
    return nvs_config_eink_clock() && idx == s_page_count;
}
static int total_pages_count(void) {
    return s_page_count + (nvs_config_eink_clock() ? 1 : 0);
}

// Wspolny rysunek biezacej strony. Wolac pod eink_lock().
// draw_diag() odswieza panel samodzielnie, reszta przez eink_refresh() nizej.
static void draw_current_locked(void) {
    rebuild_pages();
    int total = total_pages_count();
    if (total == 0) { draw_diag(); return; }
    if (s_cur_page < 0 || s_cur_page >= total) s_cur_page = 0;
    if (is_clock_page(s_cur_page)) draw_clock_page(s_cur_page + 1, total);
    else                           draw_meter_page(s_page_ids[s_cur_page], s_cur_page + 1, total);
    eink_refresh();
}

void display_eink_refresh_pages(void) {
    eink_lock();
    draw_current_locked();
    eink_unlock();
}

void display_eink_next_page(void) {
    eink_lock();
    rebuild_pages();
    int total = total_pages_count();
    if (total == 0) { draw_diag(); eink_unlock(); return; }
    s_cur_page = (s_cur_page + 1) % total;
    draw_current_locked();
    eink_unlock();
}

void display_eink_first_page(void) {
    eink_lock();
    s_cur_page = 0;
    draw_current_locked();
    eink_unlock();
}

// ---------- przycisk BOOT + auto-odswiezanie ----------

static int s_btn_pin = -1;

// Task przycisku: krotkie nacisniecie = nastepna strona, dlugie = pierwsza.
static TaskHandle_t s_btn_task_h = NULL;
static TaskHandle_t s_refresh_task_h = NULL;
static volatile bool s_eink_paused = false;

static void button_task(void *arg) {
    (void)arg;
    const int LONG_MS = 1000;   // prog dlugiego nacisniecia
    bool prev_up = true;        // przycisk zwolniony (pullup, stan wysoki)
    while (1) {
        int level = gpio_get_level(s_btn_pin);
        bool down = (level == 0); // aktywny w stanie niskim
        if (down && prev_up) {
            // poczatek nacisniecia - mierz czas trzymania
            int held = 0;
            while (gpio_get_level(s_btn_pin) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                held += 20;
                if (held > 5000) break;
            }
            if (held >= LONG_MS) display_eink_first_page();
            else if (held >= 40)  display_eink_next_page();  // odfiltruj drgania <40ms
            prev_up = false;
        } else if (!down) {
            prev_up = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Task auto-odswiezania: co 60 s odswiez biezaca strone (nowe dane z historii).
// Budzik taska odswiezania: pozwala odswiezyc ekran od razu po zmianie
// ustawien, bez dotykania SPI z taska HTTP.
static SemaphoreHandle_t s_wake = NULL;

void display_eink_wake(void) {
    if (s_wake) xSemaphoreGive(s_wake);
}

static void refresh_task(void *arg) {
    (void)arg;
    // Pierwsze odswiezenie po 15 s (daj czas na pierwsze ramki i SNTP).
    vTaskDelay(pdMS_TO_TICKS(15000));
    while (1) {
        if (!s_eink_paused) display_eink_refresh_pages();

        // Domyslnie cykl 60 s. Gdy na ekranie jest zegar, spimy do najblizszej
        // pelnej minuty - inaczej cyfra minut zmienialaby sie w losowym momencie
        // cyklu i potrafila spoznic sie o niemal cala minute.
        int wait_ms = 60000;
        if (is_clock_page(s_cur_page)) {
            time_t now = time(NULL);
            if (now >= TIME_SYNCED_MIN) {
                wait_ms = (int)(60 - (now % 60)) * 1000;
                // Tuz po pelnej minucie (rysowanie tez trwa) - czekaj na nastepna,
                // zeby nie odswiezac panelu dwa razy pod rzad.
                if (wait_ms < 2000) wait_ms += 60000;
            }
        }
        if (s_wake) xSemaphoreTake(s_wake, pdMS_TO_TICKS(wait_ms));
        else        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void display_eink_start_tasks(int pin_button) {
    s_btn_pin = pin_button;
    gpio_config_t btn = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pin_bit_mask = (1ULL << pin_button),
    };
    gpio_config(&btn);
    if (!s_wake) s_wake = xSemaphoreCreateBinary();
    xTaskCreate(button_task, "eink_btn", 3072, NULL, 3, &s_btn_task_h);
    xTaskCreate(refresh_task, "eink_refresh", 4096, NULL, 2, &s_refresh_task_h);
    ESP_LOGI(TAG, "Tasks e-ink uruchomione (przycisk GPIO%d)", pin_button);
}

void display_eink_pause(void) {
    s_eink_paused = true;
    // Poczekaj az trwajace odswiezanie sie skonczy i dopiero wtedy usun taski.
    // vTaskDelete na tasku bedacym w srodku spi_device_acquire_bus zostawialby
    // magistrale SPI2 przejeta na zawsze (deadlock CC1101 i kolejnych operacji).
    // Task zablokowany na tym mutexie mozna bezpiecznie usunac.
    eink_lock();
    if (s_refresh_task_h) { vTaskDelete(s_refresh_task_h); s_refresh_task_h = NULL; }
    if (s_btn_task_h)     { vTaskDelete(s_btn_task_h);     s_btn_task_h = NULL; }
    eink_unlock();
    ESP_LOGI(TAG, "Taski e-ink wstrzymane (OTA)");
}

// Wznow e-ink po wstrzymaniu (np. gdy OTA przerwane bo wersja aktualna).
// Odtwarza taski przyciskow i odswiezania uzywajac zapamietanego pinu.
void display_eink_resume(void) {
    if (s_btn_task_h || s_refresh_task_h) return;  // juz dzialaja
    s_eink_paused = false;
    if (s_btn_pin >= 0) {
        xTaskCreate(button_task, "eink_btn", 3072, NULL, 3, &s_btn_task_h);
    }
    xTaskCreate(refresh_task, "eink_refresh", 4096, NULL, 2, &s_refresh_task_h);
    ESP_LOGI(TAG, "Taski e-ink wznowione");
}
