#include "cc1101.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CC1101";

// ── Rejestry CC1101 ────────────────────────────────────────
#define R_IOCFG2    0x00
#define R_IOCFG1    0x01
#define R_IOCFG0    0x02
#define R_FIFOTHR   0x03
#define R_SYNC1     0x04
#define R_SYNC0     0x05
#define R_PKTLEN    0x06
#define R_PKTCTRL1  0x07
#define R_PKTCTRL0  0x08
#define R_ADDR      0x09
#define R_CHANNR    0x0A
#define R_FSCTRL1   0x0B
#define R_FSCTRL0   0x0C
#define R_FREQ2     0x0D
#define R_FREQ1     0x0E
#define R_FREQ0     0x0F
#define R_MDMCFG4   0x10
#define R_MDMCFG3   0x11
#define R_MDMCFG2   0x12
#define R_MDMCFG1   0x13
#define R_MDMCFG0   0x14
#define R_DEVIATN   0x15
#define R_MCSM2     0x16
#define R_MCSM1     0x17
#define R_MCSM0     0x18
#define R_FOCCFG    0x19
#define R_BSCFG     0x1A
#define R_AGCCTRL2  0x1B
#define R_AGCCTRL1  0x1C
#define R_AGCCTRL0  0x1D
#define R_WOREVT1   0x1E
#define R_WOREVT0   0x1F
#define R_WORCTRL   0x20
#define R_FREND1    0x21
#define R_FREND0    0x22
#define R_FSCAL3    0x23
#define R_FSCAL2    0x24
#define R_FSCAL1    0x25
#define R_FSCAL0    0x26
#define R_RCCTRL1   0x27
#define R_RCCTRL0   0x28
#define R_FSTEST    0x29
#define R_PTEST     0x2A
#define R_AGCTEST   0x2B
#define R_TEST2     0x2C
#define R_TEST1     0x2D
#define R_TEST0     0x2E

// ── Stroby ─────────────────────────────────────────────────
#define S_SRES      0x30
#define S_SRX       0x34
#define S_SIDLE     0x36
#define S_SFTX      0x3B
#define S_SFRX      0x3A
#define S_SCAL      0x33
#define S_SNOP      0x3D

// ── Status (burst read 0xC0) ───────────────────────────────
#define ST_PARTNUM    0x30
#define ST_VERSION    0x31
#define ST_RSSI       0x34
#define ST_MARCSTATE  0x35
#define ST_RXBYTES    0x3B

#define ST_TXBYTES    0x3A
#define ST_PKTSTATUS  0x38

// Stale strumieniowego odbioru
#define RX_FIFO_THRESHOLD 10
#define MAX_FRAME_SIZE    450   // bylo 290; Amiplus 3-faz zakodowany 3of6 = 326B
#define MARC_RETRY        5

#define RXFIFO        0x3F
#define WRITE_BURST   0x40
#define READ_SINGLE   0x80
#define READ_BURST    0xC0

static spi_device_handle_t s_spi = NULL;
static cc1101_config_t     s_cfg = {0};
static wmbus_frame_cb_t    s_callback = NULL;
static TaskHandle_t        s_rx_task = NULL;
static volatile bool       s_rx_stop = false;

// ── Konfiguracja wMbus T1/C1 (z bodek85, sprawdzona) ───────
// SYNC word 0x543D — to klucz do wykrywania ramek zamiast szumu
static const uint8_t WMBUS_CFG[][2] = {
    {R_IOCFG2,   0x06}, // GDO2: sync word sent/received
    {R_IOCFG1,   0x2E},
    {R_IOCFG0,   0x00}, // GDO0: RX FIFO threshold
    {R_FIFOTHR,  0x0A},
    {R_SYNC1,    0x54}, // SYNC word high
    {R_SYNC0,    0x3D}, // SYNC word low
    {R_PKTLEN,   0xFF},
    {R_PKTCTRL1, 0x00},
    {R_PKTCTRL0, 0x00}, // Fixed length, no CRC
    {R_ADDR,     0x00},
    {R_CHANNR,   0x00},
    {R_FSCTRL1,  0x08},
    {R_FSCTRL0,  0x00},
    {R_FREQ2,    0x21}, // 868.95 MHz
    {R_FREQ1,    0x6B},
    {R_FREQ0,    0xD0},
    {R_MDMCFG4,  0x5C}, // RX BW + data rate exp
    {R_MDMCFG3,  0x04}, // data rate mantissa (~100 kbps)
    {R_MDMCFG2,  0x06}, // 2-FSK, sync 16/16
    {R_MDMCFG1,  0x22},
    {R_MDMCFG0,  0xF8},
    {R_DEVIATN,  0x44},
    {R_MCSM2,    0x07},
    {R_MCSM1,    0x00},
    {R_MCSM0,    0x18},
    {R_FOCCFG,   0x2E},
    {R_BSCFG,    0xBF},
    {R_AGCCTRL2, 0x43},
    {R_AGCCTRL1, 0x09},
    {R_AGCCTRL0, 0xB5},
    {R_WOREVT1,  0x87},
    {R_WOREVT0,  0x6B},
    {R_WORCTRL,  0xFB},
    {R_FREND1,   0xB6},
    {R_FREND0,   0x10},
    {R_FSCAL3,   0xEA},
    {R_FSCAL2,   0x2A},
    {R_FSCAL1,   0x00},
    {R_FSCAL0,   0x1F},
    {R_RCCTRL1,  0x41},
    {R_RCCTRL0,  0x00},
    {R_FSTEST,   0x59},
    {R_PTEST,    0x7F},
    {R_AGCTEST,  0x3F},
    {R_TEST2,    0x81},
    {R_TEST1,    0x35},
    {R_TEST0,    0x09},
};

// ── SPI helpers ────────────────────────────────────────────
static uint8_t spi_rw(uint8_t addr, uint8_t val) {
    spi_transaction_t t = {
        .flags   = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length  = 16,
        .tx_data = {addr, val},
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return t.rx_data[1];
}

static void write_reg(uint8_t addr, uint8_t val) { spi_rw(addr, val); }

static uint8_t read_reg(uint8_t addr) {
    return spi_rw(addr | READ_SINGLE, 0x00);
}

static uint8_t read_status(uint8_t addr) {
    return spi_rw(addr | READ_BURST, 0x00);
}

static uint8_t strobe(uint8_t cmd) {
    spi_transaction_t t = {
        .flags   = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length  = 8,
        .tx_data = {cmd},
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return t.rx_data[0];
}

static void read_fifo_burst(uint8_t *buf, size_t len) {
    if (len == 0) return;
    uint8_t *tx = calloc(1, len + 1);
    uint8_t *rx = calloc(1, len + 1);
    tx[0] = RXFIFO | READ_BURST;
    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    memcpy(buf, rx + 1, len);
    free(tx);
    free(rx);
}

static int8_t convert_rssi(uint8_t raw) {
    int16_t r = raw;
    if (r >= 128) r -= 256;
    return (int8_t)((r / 2) - 74);
}

// ── Konfiguracja radia ─────────────────────────────────────
static void configure_wmbus(void) {
    strobe(S_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t partnum = read_status(ST_PARTNUM);
    uint8_t version = read_status(ST_VERSION);
    ESP_LOGI(TAG, "PARTNUM=0x%02X VERSION=0x%02X (oczekiwane 0x00 / 0x04|0x14)",
             partnum, version);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGE(TAG, "CC1101 nie wykryty! Sprawdz SPI (CS/MOSI/MISO/SCK/VCC/GND)");
    }

    for (size_t i = 0; i < sizeof(WMBUS_CFG)/sizeof(WMBUS_CFG[0]); i++)
        write_reg(WMBUS_CFG[i][0], WMBUS_CFG[i][1]);

    // Nadpisz częstotliwość jeśli użytkownik podał inną niż domyślna
    if (s_cfg.freq_mhz > 0.1f) {
        uint32_t fw = (uint32_t)((s_cfg.freq_mhz * 65536.0f) / 26.0f);
        write_reg(R_FREQ2, (fw >> 16) & 0xFF);
        write_reg(R_FREQ1, (fw >>  8) & 0xFF);
        write_reg(R_FREQ0, (fw      ) & 0xFF);
    }

    ESP_LOGI(TAG, "CC1101 skonfigurowany dla wMbus @ %.3f MHz", s_cfg.freq_mhz);
}

// ── Dekodowanie 3-of-6 (tryb T1) ───────────────────────────
// Zwraca liczbę zdekodowanych bajtów lub 0 przy błędzie
// Rozmiar zakodowany 3of6 dla N bajtow zdekodowanych:
// kazde 2 bajty danych = 3 bajty zakodowane (4 nibble x 6 bitow = 24b)
static size_t encoded_size_3of6(size_t decoded_size) {
    return (3 * decoded_size + 1) / 2;
}

// Oblicz pelna dlugosc ramki wMbus (z CRC) na podstawie L-field
static size_t wmbus_frame_size(uint8_t l_field) {
    // 2 pierwsze bloki = 25 bajtow (bez CRC i L), kolejne po 16
    size_t nrBlocks = (l_field < 26) ? 2 : ((l_field - 26) / 16 + 3);
    // L-field + 1 (samo L) + 2 bajty CRC na kazdy blok
    return l_field + 1 + 2 * nrBlocks;
}

static int decode_3of6(const uint8_t *coded, size_t coded_len,
                       uint8_t *out, size_t out_max) {
    static const int8_t lut[64] = {
        [0b010110]=0x0, [0b001101]=0x1, [0b001110]=0x2, [0b001011]=0x3,
        [0b011100]=0x4, [0b011001]=0x5, [0b011010]=0x6, [0b010011]=0x7,
        [0b101100]=0x8, [0b100101]=0x9, [0b100110]=0xA, [0b100011]=0xB,
        [0b110100]=0xC, [0b110001]=0xD, [0b110010]=0xE, [0b101001]=0xF,
    };
    // Tablica: domyślnie 0 to też 0x0, więc oznaczmy nieprawidłowe jako -1
    static int8_t lut_valid[64];
    static bool lut_init = false;
    if (!lut_init) {
        for (int i = 0; i < 64; i++) lut_valid[i] = -1;
        lut_valid[0b010110]=0x0; lut_valid[0b001101]=0x1;
        lut_valid[0b001110]=0x2; lut_valid[0b001011]=0x3;
        lut_valid[0b011100]=0x4; lut_valid[0b011001]=0x5;
        lut_valid[0b011010]=0x6; lut_valid[0b010011]=0x7;
        lut_valid[0b101100]=0x8; lut_valid[0b100101]=0x9;
        lut_valid[0b100110]=0xA; lut_valid[0b100011]=0xB;
        lut_valid[0b110100]=0xC; lut_valid[0b110001]=0xD;
        lut_valid[0b110010]=0xE; lut_valid[0b101001]=0xF;
        lut_init = true;
    }
    (void)lut;

    size_t segments = coded_len * 8 / 6;
    int out_len = 0;
    for (size_t i = 0; i < segments; i++) {
        size_t bit_idx    = i * 6;
        size_t byte_idx   = bit_idx / 8;
        size_t bit_offset = bit_idx % 8;

        uint8_t code = coded[byte_idx] << bit_offset;
        if (bit_offset > 0 && byte_idx + 1 < coded_len)
            code |= coded[byte_idx + 1] >> (8 - bit_offset);
        code >>= 2;

        int8_t nibble = lut_valid[code & 0x3F];
        if (nibble < 0) return 0; // nieprawidłowy kod

        if (i % 2 == 0) {
            if (out_len >= (int)out_max) break;
            out[out_len] = nibble << 4;
        } else {
            out[out_len] |= nibble;
            out_len++;
        }
    }
    return out_len;
}

// ── Task odbioru ───────────────────────────────────────────
// ─────────── Strumieniowy odbior wMbus (wzor: bodek85/SzczepanLeon) ───────────
// Oblicz dlugosc zdekodowanej ramki mode A z L-field
static size_t mode_a_decoded_size(uint8_t l){
    size_t nb = (l < 26) ? 2 : ((l - 26)/16 + 3);
    return l + 1 + 2*nb;
}
// Ile bajtow zakodowanych 3of6 dla mode T
static size_t mode_t_packet_size(uint8_t l){
    return encoded_size_3of6(mode_a_decoded_size(l));
}

// Odczyt N bajtow z RX FIFO (burst) do bufora
static void rx_fifo_read(uint8_t *dst, size_t n){
    if(n) read_fifo_burst(dst, n);
}

static void rx_task(void *arg) {
    uint8_t  rxbuf[MAX_FRAME_SIZE];
    uint8_t  decoded[MAX_FRAME_SIZE];

    while (1) {
        if (s_rx_stop) {           // bezpieczne zatrzymanie (np. przed OTA)
            strobe(S_SIDLE);
            s_rx_task = NULL;
            vTaskDelete(NULL);
            return;
        }
        // ---- INIT_RX: reset, czyszczenie FIFO, wejscie w RX, infinite length ----
        strobe(S_SIDLE);
        strobe(S_SFTX);
        strobe(S_SFRX);
        write_reg(R_FIFOTHR, 0x0A);
        write_reg(R_PKTCTRL0, 0x02);   // infinite length na start
        size_t  rxlen = 0, expected = 0;
        uint8_t l_field = 0;
        strobe(S_SRX);

        // ---- WAIT_FOR_SYNC: czekaj na naglowek (>=4 bajty w FIFO) ----
        int waited = 0;
        bool got_header = false;
        while (waited < 400) {
            uint8_t st = read_status(ST_RXBYTES);
            if (st == 0xFF) break;
            if (st & 0x80) { strobe(S_SFRX); break; } // overflow
            if ((st & 0x7F) >= 4) { got_header = true; break; }
            vTaskDelay(pdMS_TO_TICKS(5));
            waited++;
        }
        if (!got_header) continue;

        // ---- Naglowek: odczytaj 3 bajty, zdekoduj 3of6 by poznac L-field ----
        uint8_t header[4];
        rx_fifo_read(header, 4);
        uint8_t hdec[4];
        int hn = decode_3of6(header, 3, hdec, sizeof(hdec));
        if (hn < 1) {
            // moze tryb C (0x54 preamble) — nie obslugujemy tu, restart
            continue;
        }
        l_field = hdec[0];
        if (l_field < 10 || l_field > 250) continue;  // nieprawidlowa dlugosc
        expected = mode_t_packet_size(l_field);
        if (expected > MAX_FRAME_SIZE) expected = MAX_FRAME_SIZE;

        // Naglowek (4 zakodowane bajty) trafiaja do bufora
        memcpy(rxbuf, header, 4);
        rxlen = 4;

        // ---- Przelacz na FIXED length gdy znamy dlugosc ----
        if (expected < 256) {
            write_reg(R_PKTLEN, (uint8_t)expected);
            write_reg(R_PKTCTRL0, 0x00); // fixed
        }
        write_reg(R_FIFOTHR, RX_FIFO_THRESHOLD);

        // ---- READ_DATA: opróżniaj FIFO porcjami az do expected ----
        int8_t rssi = convert_rssi(read_status(ST_RSSI));
        int guard = 0;
        bool complete = false;
        while (guard++ < 200) {
            uint8_t st = read_status(ST_RXBYTES);
            if (st == 0xFF) break;
            if (st & 0x80) { strobe(S_SFRX); break; } // overflow -> porzuc
            uint8_t nfifo = st & 0x7F;
            uint8_t marc  = read_status(ST_MARCSTATE) & 0x1F;

            size_t remaining = expected - rxlen;

            if (nfifo > 0) {
                // Zostaw 1 bajt w FIFO podczas odbioru (errata), chyba ze to koniec
                size_t to_read;
                if (remaining <= nfifo) to_read = remaining;
                else to_read = (nfifo > 1) ? (nfifo - 1) : 0;
                if (to_read > 0 && rxlen + to_read <= MAX_FRAME_SIZE) {
                    rx_fifo_read(rxbuf + rxlen, to_read);
                    rxlen += to_read;
                }
            }

            if (rxlen >= expected) { complete = true; break; }

            // Fallback: chip wrocil do IDLE/RX_END a my mamy juz dane -> dociagnij reszte
            if ((marc == 0x01 || marc == 0x0E) && rxlen > 4) {
                for (int r = 0; r < MARC_RETRY && rxlen < expected; r++) {
                    uint8_t rem = read_status(ST_RXBYTES) & 0x7F;
                    if (rem > 0) {
                        size_t td = (expected - rxlen < rem) ? (expected - rxlen) : rem;
                        if (rxlen + td <= MAX_FRAME_SIZE) {
                            rx_fifo_read(rxbuf + rxlen, td);
                            rxlen += td;
                        }
                    } else {
                        esp_rom_delay_us(200);
                    }
                }
                if (rxlen >= expected) complete = true;
                break;
            }
            // Przy FreeRTOS 1000Hz tick=1ms: oddajemy CPU co 1ms.
            // W 1ms przychodzi ~12B (przy 100kbps), FIFO 64B ma duzy zapas.
            // Czytamy co iteracje (powyzej), tu tylko oddajemy CPU - bez busy-wait,
            // bez ryzyka overflow, serwer HTTP pozostaje responsywny.
            vTaskDelay(1);
        }
        strobe(S_SFRX);

        if (!complete || rxlen < expected) {
            ESP_LOGW(TAG, "T1: ramka niepelna %d/%d (L=%d rssi=%d)",
                     (int)rxlen, (int)expected, l_field, rssi);
            continue;
        }

        // ---- Dekoduj cala ramke 3of6 ----
        int dlen = decode_3of6(rxbuf, rxlen, decoded, sizeof(decoded));
        if (dlen <= 0) {
            ESP_LOGW(TAG, "T1: blad 3of6 po odbiorze (L=%d rxlen=%d)", l_field, (int)rxlen);
            continue;
        }

        wmbus_frame_t frame = {0};
        frame.rssi = rssi;
        size_t flen = mode_a_decoded_size(l_field);
        if ((size_t)dlen < flen) flen = dlen;
        if (flen > sizeof(frame.data)) flen = sizeof(frame.data);
        memcpy(frame.data, decoded, flen);
        frame.len = flen;

        ESP_LOGD(TAG, "T1 OK: L=%d frame=%dB decoded=%dB rssi=%d",
                 l_field, (int)flen, dlen, rssi);

        if (frame.len >= 10 && s_callback)
            s_callback(&frame);

        // Diagnostyka stosu rx_task co 50 ramek (weryfikacja zapasu po zmniejszeniu).
        static int s_frame_cnt = 0;
        if (++s_frame_cnt % 50 == 0) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "rx_task min. wolny stos: %u B", (unsigned)(hw * sizeof(StackType_t)));
        }
    }
}

void cc1101_init(const cc1101_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(cc1101_config_t));

    spi_bus_config_t bus = {
        .mosi_io_num   = cfg->pin_mosi,
        .miso_io_num   = cfg->pin_miso,
        .sclk_io_num   = cfg->pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = cfg->pin_cs,
        .queue_size     = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &s_spi));

    // GDO piny jako wejścia (na razie polling, nie przerwania)
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->pin_gdo0) | (1ULL << cfg->pin_gdo2),
        .mode         = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io);

    configure_wmbus();
    ESP_LOGI(TAG, "CC1101 init OK");
}

void cc1101_start_receive(wmbus_frame_cb_t callback) {
    s_callback = callback;
    s_rx_stop = false;
    xTaskCreate(rx_task, "cc1101_rx", 10240, NULL, 7, &s_rx_task);
    ESP_LOGI(TAG, "Odbior wMbus uruchomiony");
}

void cc1101_stop(void) {
    if (!s_rx_task) { strobe(S_SIDLE); return; }
    // Popros task by sam wyszedl w bezpiecznym miejscu (nie w trakcie SPI)
    s_rx_stop = true;
    // Czekaj do 2s az task sie zakonczy
    for (int i = 0; i < 200 && s_rx_task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    // Gdyby utknal — wymus
    if (s_rx_task) { vTaskDelete(s_rx_task); s_rx_task = NULL; }
    s_rx_stop = false;
    strobe(S_SIDLE);
}

int8_t cc1101_get_rssi(void) {
    return convert_rssi(read_status(ST_RSSI));
}
