#include "cc1101.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "CC1101";

// Rejestry CC1101
#define CC1101_IOCFG0   0x00
#define CC1101_IOCFG2   0x01
#define CC1101_FIFOTHR  0x03
#define CC1101_SYNC1    0x04
#define CC1101_SYNC0    0x05
#define CC1101_PKTLEN   0x06
#define CC1101_PKTCTRL0 0x08
#define CC1101_FSCTRL1  0x0B
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_MDMCFG1  0x13
#define CC1101_MDMCFG0  0x14
#define CC1101_DEVIATN  0x15
#define CC1101_MCSM0    0x18
#define CC1101_FOCCFG   0x19
#define CC1101_AGCCTRL2 0x1B
#define CC1101_AGCCTRL1 0x1C
#define CC1101_AGCCTRL0 0x1D
#define CC1101_FREND1   0x21
#define CC1101_FSCAL3   0x23
#define CC1101_FSCAL2   0x24
#define CC1101_FSCAL1   0x25
#define CC1101_FSCAL0   0x26
#define CC1101_TEST2    0x2C
#define CC1101_TEST1    0x2D
#define CC1101_TEST0    0x2E

// Stroby
#define CC1101_SRES     0x30
#define CC1101_SRX      0x34
#define CC1101_SIDLE    0x36
#define CC1101_SFRX     0x3A
#define CC1101_RXFIFO   0xBF  // burst read

// Status
#define CC1101_RXBYTES  0xFB
#define CC1101_RSSI     0xF4
#define CC1101_PKTSTATUS 0xF8

static spi_device_handle_t s_spi = NULL;
static cc1101_config_t     s_cfg = {0};
static wmbus_frame_cb_t    s_callback = NULL;
static QueueHandle_t       s_frame_queue = NULL;
static TaskHandle_t        s_rx_task = NULL;

static uint8_t spi_transfer(uint8_t addr, uint8_t val) {
    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = {addr, val},
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return t.rx_data[1];
}

static void write_reg(uint8_t addr, uint8_t val) {
    spi_transfer(addr, val);
}

static uint8_t read_reg(uint8_t addr) {
    return spi_transfer(addr | 0x80, 0x00);
}

static uint8_t strobe(uint8_t cmd) {
    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 8,
        .tx_data = {cmd},
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return t.rx_data[0];
}

static void read_burst(uint8_t addr, uint8_t *buf, size_t len) {
    spi_transaction_t t = {
        .flags   = 0,
        .addr    = addr,
        .length  = len * 8,
        .rx_buffer = buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

// Konfiguracja T1/C1 868.95 MHz, 32.768 kbps
static void configure_wmbus(void) {
    strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    write_reg(CC1101_IOCFG0,   0x2E); // GDO0 — High impedance
    write_reg(CC1101_IOCFG2,   0x0B); // GDO2 — Serial clock
    write_reg(CC1101_FIFOTHR,  0x47);
    write_reg(CC1101_PKTCTRL0, 0x00); // Fixed packet length
    write_reg(CC1101_FSCTRL1,  0x06);
    write_reg(CC1101_MDMCFG4,  0x7A); // BW 200kHz, dr exponent
    write_reg(CC1101_MDMCFG3,  0x83); // Data rate 32.768 kbps
    write_reg(CC1101_MDMCFG2,  0x30); // 2-FSK, no sync
    write_reg(CC1101_MDMCFG1,  0x22);
    write_reg(CC1101_MDMCFG0,  0xF8);
    write_reg(CC1101_DEVIATN,  0x50); // ±47.6 kHz
    write_reg(CC1101_MCSM0,    0x18);
    write_reg(CC1101_FOCCFG,   0x16);
    write_reg(CC1101_AGCCTRL2, 0x43);
    write_reg(CC1101_AGCCTRL1, 0x40);
    write_reg(CC1101_AGCCTRL0, 0x91);
    write_reg(CC1101_FREND1,   0x56);
    write_reg(CC1101_FSCAL3,   0xE9);
    write_reg(CC1101_FSCAL2,   0x2A);
    write_reg(CC1101_FSCAL1,   0x00);
    write_reg(CC1101_FSCAL0,   0x1F);
    write_reg(CC1101_TEST2,    0x81);
    write_reg(CC1101_TEST1,    0x35);
    write_reg(CC1101_TEST0,    0x09);

    // Częstotliwość 868.95 MHz
    // Fcarrier = Fxosc * FREQ / 2^16, Fxosc = 26 MHz
    uint32_t freq_word = (uint32_t)((s_cfg.freq_mhz * 65536.0f) / 26.0f);
    write_reg(CC1101_FREQ2, (freq_word >> 16) & 0xFF);
    write_reg(CC1101_FREQ1, (freq_word >>  8) & 0xFF);
    write_reg(CC1101_FREQ0, (freq_word      ) & 0xFF);

    ESP_LOGI(TAG, "CC1101 skonfigurowany @ %.3f MHz", s_cfg.freq_mhz);
}

static int8_t convert_rssi(uint8_t rssi_raw) {
    if (rssi_raw >= 128) return (int8_t)((rssi_raw - 256) / 2) - 74;
    return (int8_t)(rssi_raw / 2) - 74;
}

static void rx_task(void *arg) {
    while (1) {
        strobe(CC1101_SRX);
        // Czekaj na dane w FIFO (polling GDO0 lub RXBYTES)
        // W pełnej implementacji używamy przerwania na GDO0
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t rxbytes = read_reg(CC1101_RXBYTES);
        if (rxbytes == 0 || rxbytes > 128) continue;

        wmbus_frame_t frame = {0};
        frame.len  = rxbytes;
        frame.rssi = convert_rssi(read_reg(CC1101_RSSI));
        read_burst(CC1101_RXFIFO, frame.data, rxbytes);

        if (s_callback) s_callback(&frame);
        strobe(CC1101_SFRX);
    }
}

void cc1101_init(const cc1101_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(cc1101_config_t));

    spi_bus_config_t bus = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = cfg->pin_miso,
        .sclk_io_num = cfg->pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = cfg->pin_cs,
        .queue_size     = 8,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &s_spi));

    configure_wmbus();
    ESP_LOGI(TAG, "CC1101 init OK");
}

void cc1101_start_receive(wmbus_frame_cb_t callback) {
    s_callback = callback;
    xTaskCreate(rx_task, "cc1101_rx", 4096, NULL, 10, &s_rx_task);
    ESP_LOGI(TAG, "Odbiór wMbus uruchomiony");
}

void cc1101_stop(void) {
    if (s_rx_task) vTaskDelete(s_rx_task);
    strobe(CC1101_SIDLE);
}

int8_t cc1101_get_rssi(void) {
    return convert_rssi(read_reg(CC1101_RSSI));
}
