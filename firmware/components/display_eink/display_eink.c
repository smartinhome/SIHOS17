#include "display_eink.h"
#include "qr_data.h"
#include "logo_data.h"
#include "font_data.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    spi_device_polling_transmit(s_spi, &t);
}

static void eink_data(uint8_t d) {
    gpio_set_level(s_cfg.pin_dc, 1); // data
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &d;
    spi_device_polling_transmit(s_spi, &t);
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
        spi_device_polling_transmit(s_spi, &t);
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

static void eink_panel_init(void) {
    eink_reset();
    eink_wait_busy();

    eink_cmd(0x12); // SWRESET
    eink_wait_busy();

    eink_cmd(0x01); // Driver output control
    eink_data((PANEL_H - 1) & 0xFF);
    eink_data(((PANEL_H - 1) >> 8) & 0xFF);
    eink_data(0x00);

    eink_cmd(0x11); // data entry mode
    eink_data(0x03); // X inc, Y inc

    eink_cmd(0x44); // set RAM X start/end (w bajtach)
    eink_data(0x00);
    eink_data(PANEL_BPR - 1); // 15

    eink_cmd(0x45); // set RAM Y start/end
    eink_data(0x00);
    eink_data(0x00);
    eink_data((PANEL_H - 1) & 0xFF);
    eink_data(((PANEL_H - 1) >> 8) & 0xFF);

    eink_cmd(0x3C); // border waveform
    eink_data(0x05);

    eink_cmd(0x21); // display update control
    eink_data(0x00);
    eink_data(0x80);

    eink_cmd(0x18); // temperature sensor
    eink_data(0x80);

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
    eink_set_cursor();
    eink_cmd(0x24); // write RAM (BW)
    eink_data_buf(s_fb, FB_SIZE);

    eink_cmd(0x22); // display update control 2
    eink_data(0xF7); // full update sequence
    eink_cmd(0x20); // master activation
    eink_wait_busy();
}

static void eink_sleep(void) {
    eink_cmd(0x10); // deep sleep
    eink_data(0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
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
    // Rotacja 270 stopni: logiczne (lx,ly) -> fizyczne (px,py)
    // ESPHome rotation 270: px = ly, py = (LCD_W-1) - lx
    int px = ly;
    int py = (LCD_W - 1) - lx;
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

// ---------- API ----------

bool display_eink_init(const display_eink_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(*cfg));

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

    eink_full_refresh();
    eink_sleep();
    ESP_LOGI(TAG, "Splash (logo + napis + QR) wyswietlony");
}
