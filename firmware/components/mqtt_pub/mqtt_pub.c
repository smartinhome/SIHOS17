#include "mqtt_pub.h"
#include "nvs_config.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT";

// Kolejka miedzy zadaniem odbioru ramek a zadaniem publikujacym. Zapis do
// brokera potrafi trwac dziesiatki milisekund, a zadanie CC1101 ma priorytet 7
// i nie moze na to czekac - inaczej gubilyby sie ramki.
// Amiplus potrafi dac 20 pol w jednej ramce. Przy kolejce 24 wypychal z niej
// wszystkie inne liczniki, zanim zadanie publikujace zdazylo je wyslac -
// w Home Assistant pojawial sie wtedy tylko on.
#define MQ_LEN        96
#define TOPIC_MAX     96
#define PAYLOAD_MAX   32

typedef struct {
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
    bool retain;
} mq_item_t;

// Opis pola do ogloszenia w Home Assistant - zeby encja miala wlasciwa
// jednostke i klase, a nie byla golym tekstem.
typedef struct {
    char     id_hex[12];
    char     field[24];
    char     unit[8];
    uint32_t last_ms;     // kiedy ostatnio opublikowano to pole
} ha_item_t;

#define HA_MAX 24

// Najkrotszy odstep miedzy publikacjami TEGO SAMEGO pola. Otus nadaje co
// kilkanascie sekund i ma ~20 pol - bez tego brokera zalewaloby kilkaset
// wiadomosci na minute, a wartosci i tak zmieniaja sie wolno.
#define MIN_INTERVAL_MS 15000
static ha_item_t     s_ha[HA_MAX];
static int           s_ha_count = 0;

static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;
static volatile bool s_connected = false;
static volatile bool s_running   = false;
static uint32_t      s_sent = 0, s_failed = 0;

// ---------- pomocnicze ----------

static void prefix_of(char *out, size_t cap) {
    const sih_config_t *c = nvs_config_ptr();
    const char *p = c->mqtt_prefix[0] ? c->mqtt_prefix : "sihos17";
    snprintf(out, cap, "%s", p);
}

// Klasa urzadzenia i jednostka dla Home Assistant na podstawie jednostki pola.
// Home Assistant sprawdza jednostke wzgledem klasy urzadzenia i ODRZUCA encje,
// gdy sie nie zgadza. Dla klasy "water" dopuszcza m3 wylacznie zapisane jako
// "m3" z indeksem gornym - nasze wewnetrzne "m3" bylo odrzucane i wodomierze
// w ogole nie pojawialy sie w HA.
static const char *ha_unit(const char *unit) {
    if (!unit) return "";
    if (strcmp(unit, "m3") == 0)  return "m\u00b3";     // m3 z indeksem gornym
    if (strcmp(unit, "kVARh") == 0) return "kvarh";
    if (strcmp(unit, "VAR") == 0)   return "var";
    return unit;
}

static void ha_class_for(const char *unit, const char **dev_class,
                         const char **state_class) {
    if (strcmp(unit, "kWh") == 0)      { *dev_class = "energy";      *state_class = "total_increasing"; }
    else if (strcmp(unit, "m3") == 0)  { *dev_class = "water";       *state_class = "total_increasing"; }
    else if (strcmp(unit, "kW") == 0)  { *dev_class = "power";       *state_class = "measurement"; }
    else if (strcmp(unit, "V") == 0)   { *dev_class = "voltage";     *state_class = "measurement"; }
    else if (strcmp(unit, "A") == 0)   { *dev_class = "current";     *state_class = "measurement"; }
    else if (strcmp(unit, "VAR") == 0 ||
             strcmp(unit, "kVARh") == 0) { *dev_class = "";          *state_class = "measurement"; }
    else                               { *dev_class = "";            *state_class = ""; }
}

// Kolejkuj wiadomosc. Gdy kolejka pelna - odrzuc najstarsza zamiast blokowac.
static void mq_push(const char *topic, const char *payload, bool retain) {
    if (!s_queue) return;
    mq_item_t it;
    snprintf(it.topic, sizeof(it.topic), "%s", topic);
    snprintf(it.payload, sizeof(it.payload), "%s", payload);
    it.retain = retain;
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        mq_item_t drop;
        xQueueReceive(s_queue, &drop, 0);      // zwolnij miejsce
        xQueueSend(s_queue, &it, 0);
    }
}

// ---------- ogloszenia Home Assistant ----------

static void ha_announce_one(const ha_item_t *h) {
    const sih_config_t *c = nvs_config_ptr();
    if (!c->mqtt_ha_discovery) return;
    char pref[24]; prefix_of(pref, sizeof(pref));
    const char *dc = "", *sc = "";
    ha_class_for(h->unit, &dc, &sc);

    char topic[TOPIC_MAX];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s_%s/config",
             pref, h->id_hex, h->field);

    // Wlasna nazwa licznika, jesli ustawiona w panelu - inaczej samo ID.
    const char *nice = nvs_config_meter_name(h->id_hex);
    char devname[40];
    snprintf(devname, sizeof(devname), "%s", (nice && nice[0]) ? nice : h->id_hex);

    // Budowane recznie zamiast cJSON - komunikat jest krotki i staly,
    // a unikamy alokacji na goracej sciezce.
    char cfg[512];
    int n = snprintf(cfg, sizeof(cfg),
        "{\"name\":\"%s %s\","
        "\"uniq_id\":\"%s_%s_%s\","
        "\"stat_t\":\"%s/%s/%s\","
        "\"unit_of_meas\":\"%s\","
        "\"avty_t\":\"%s/status\","
        "\"dev\":{\"ids\":[\"%s_%s\"],\"name\":\"%s\",\"mf\":\"smartinhome.pl\",\"mdl\":\"SIHOS17\"}",
        devname, h->field,
        pref, h->id_hex, h->field,
        pref, h->id_hex, h->field,
        ha_unit(h->unit),
        pref,
        pref, h->id_hex, devname);
    if (dc[0] && n > 0 && n < (int)sizeof(cfg) - 64)
        n += snprintf(cfg + n, sizeof(cfg) - n, ",\"dev_cla\":\"%s\"", dc);
    if (sc[0] && n > 0 && n < (int)sizeof(cfg) - 64)
        n += snprintf(cfg + n, sizeof(cfg) - n, ",\"stat_cla\":\"%s\"", sc);
    if (n > 0 && n < (int)sizeof(cfg) - 4) snprintf(cfg + n, sizeof(cfg) - n, "}");

    if (s_client)
        esp_mqtt_client_publish(s_client, topic, cfg, 0, 0, 1);   // retain
}

// Zapamietaj pole i ogloś je raz. Kolejne odczyty tego samego pola nie
// generuja juz ogloszen.
static ha_item_t *ha_remember(const char *id_hex, const char *field, const char *unit) {
    for (int i = 0; i < s_ha_count; i++)
        if (strcmp(s_ha[i].id_hex, id_hex) == 0 && strcmp(s_ha[i].field, field) == 0)
            return &s_ha[i];
    if (s_ha_count >= HA_MAX) return NULL;
    ha_item_t *h = &s_ha[s_ha_count++];
    snprintf(h->id_hex, sizeof(h->id_hex), "%s", id_hex);
    snprintf(h->field,  sizeof(h->field),  "%s", field);
    snprintf(h->unit,   sizeof(h->unit),   "%s", unit ? unit : "");
    if (s_connected) ha_announce_one(h);
    return h;
}

static void ha_announce_all(void) {
    for (int i = 0; i < s_ha_count; i++) ha_announce_one(&s_ha[i]);
}

// ---------- zdarzenia klienta ----------

static void on_mqtt_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
        case MQTT_EVENT_CONNECTED: {
            s_connected = true;
            ESP_LOGI(TAG, "Polaczono z brokerem");
            char pref[24]; prefix_of(pref, sizeof(pref));
            char topic[TOPIC_MAX];
            snprintf(topic, sizeof(topic), "%s/status", pref);
            esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);
            ha_announce_all();
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Rozlaczono z brokerem");
            break;
        case MQTT_EVENT_ERROR:
            s_failed++;
            break;
        default: break;
    }
}

// ---------- zadanie publikujace ----------

static void mqtt_task(void *arg) {
    (void)arg;
    mq_item_t it;
    while (s_running) {
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (!s_connected || !s_client) { s_failed++; continue; }
        int r = esp_mqtt_client_publish(s_client, it.topic, it.payload,
                                        0, 0, it.retain ? 1 : 0);
        if (r < 0) s_failed++; else s_sent++;
    }
    vTaskDelete(NULL);
}

// ---------- API ----------

void mqtt_pub_start(void) {
    const sih_config_t *c = nvs_config_ptr();
    if (!c->mqtt_enabled || !c->mqtt_host[0]) {
        ESP_LOGI(TAG, "MQTT wylaczony w konfiguracji");
        return;
    }
    if (s_client) return;

    if (!s_queue) s_queue = xQueueCreate(MQ_LEN, sizeof(mq_item_t));
    if (!s_queue) { ESP_LOGE(TAG, "Brak pamieci na kolejke"); return; }

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", c->mqtt_host,
             (unsigned)(c->mqtt_port ? c->mqtt_port : 1883));
    char pref[24]; prefix_of(pref, sizeof(pref));
    char lwt[TOPIC_MAX];
    snprintf(lwt, sizeof(lwt), "%s/status", pref);

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    if (c->mqtt_user[0]) cfg.credentials.username = c->mqtt_user;
    if (c->mqtt_pass[0]) cfg.credentials.authentication.password = c->mqtt_pass;
    // Last Will: broker sam ogloszi "offline", gdy modul zniknie z sieci.
    cfg.session.last_will.topic  = lwt;
    cfg.session.last_will.msg    = "offline";
    cfg.session.last_will.qos    = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.keepalive        = 30;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "Nie mozna utworzyc klienta"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(s_client);

    s_running = true;
    if (!s_task)
        xTaskCreate(mqtt_task, "mqtt_pub", 4096, NULL, 4, &s_task);
    ESP_LOGI(TAG, "Klient uruchomiony: %s (prefiks %s)", uri, pref);
}

void mqtt_pub_stop(void) {
    s_running = false;
    s_connected = false;
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_task = NULL;
    ESP_LOGI(TAG, "Klient zatrzymany");
}

bool mqtt_pub_connected(void) { return s_connected; }

void mqtt_pub_field(const char *id_hex, const char *field,
                    double value, const char *unit, int8_t rssi) {
    if (!s_running || !id_hex || !field) return;
    ha_item_t *h = ha_remember(id_hex, field, unit);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (h) {
        if (h->last_ms && (uint32_t)(now_ms - h->last_ms) < MIN_INTERVAL_MS) return;
        h->last_ms = now_ms;
    }

    char pref[24]; prefix_of(pref, sizeof(pref));
    char topic[TOPIC_MAX], val[PAYLOAD_MAX];
    snprintf(topic, sizeof(topic), "%s/%s/%s", pref, id_hex, field);
    snprintf(val, sizeof(val), "%.3f", value);
    mq_push(topic, val, true);

    (void)rssi;   // RSSI publikuje mqtt_pub_rssi() raz na ramke
}

void mqtt_pub_rssi(const char *id_hex, int8_t rssi) {
    if (!s_running || !id_hex) return;
    char pref[24]; prefix_of(pref, sizeof(pref));
    char topic[TOPIC_MAX], val[PAYLOAD_MAX];
    snprintf(topic, sizeof(topic), "%s/%s/rssi", pref, id_hex);
    snprintf(val, sizeof(val), "%d", (int)rssi);
    mq_push(topic, val, false);
}

void mqtt_pub_day(const char *id_hex, const char *field,
                  const char *date_str, double value, const char *unit) {
    (void)unit;
    if (!s_running || !id_hex || !field || !date_str) return;
    char pref[24]; prefix_of(pref, sizeof(pref));
    char topic[TOPIC_MAX], val[PAYLOAD_MAX];
    snprintf(topic, sizeof(topic), "%s/%s/%s/dzien/%s", pref, id_hex, field, date_str);
    snprintf(val, sizeof(val), "%.3f", value);
    mq_push(topic, val, true);
}

void mqtt_pub_stats(uint32_t *sent, uint32_t *failed) {
    if (sent)   *sent   = s_sent;
    if (failed) *failed = s_failed;
}
