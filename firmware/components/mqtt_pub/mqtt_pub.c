#include "mqtt_pub.h"
#include "nvs_config.h"
#include "history.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <time.h>
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
    uint8_t  kind;        // beta391: rodzaj licznika z dekodera (3 = gaz)
} ha_item_t;

// beta389: tyle, ile pol moze byc przypietych do dashboardu (MAX_DASH_FIELDS).
#define HA_MAX 32

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
// Liczniki za biezaca dobe. Klucz doby = rok*400 + dzien roku, zeby zmiana
// roku tez byla wykryta (samo tm_yday zawija sie 31 grudnia).
static uint32_t      s_sent_day = 0, s_failed_day = 0;
static int           s_day_key = -1;

// Zeruje liczniki dobowe po przekroczeniu lokalnej polnocy. Wolane przy
// kazdym zliczeniu i przy odczycie, wiec reset nastepuje bez osobnego timera.
static void mqtt_roll_day(void) {
    time_t now = time(NULL);
    if (now < 1700000000) return;        // czas jeszcze niezsynchronizowany
    struct tm tmv;
    localtime_r(&now, &tmv);
    int key = tmv.tm_year * 400 + tmv.tm_yday;
    if (s_day_key < 0) { s_day_key = key; return; }
    if (key != s_day_key) {
        s_day_key    = key;
        s_sent_day   = 0;
        s_failed_day = 0;
    }
}

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

static void ha_class_for(const char *unit, int kind, const char **dev_class,
                         const char **state_class) {
    if (strcmp(unit, "kWh") == 0)      { *dev_class = "energy";      *state_class = "total_increasing"; }
    // beta391: licznik gazu tez podaje m3 - bez rodzaju trafial do HA jako woda.
    else if (strcmp(unit, "m3") == 0)  { *dev_class = kind == 3 ? "gas" : "water";
                                         *state_class = "total_increasing"; }
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
    ha_class_for(h->unit, h->kind, &dc, &sc);
    // beta375: pola wyliczane nie maja jednostki, wiec klasa musi isc z nazwy.
    // Bez state_class Home Assistant nie prowadzi dla nich statystyk.
    if (strcmp(h->field, "cos_fi") == 0) { dc = "power_factor"; sc = "measurement"; }
    else if (strcmp(h->field, "tg_fi") == 0) { dc = ""; sc = "measurement"; }

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

// beta389: publikujemy tylko pola przypiete do dashboardu lub sledzone w
// historii. Dashboard bez recznie przypietych pol pokazuje pola z historii,
// wiec ta suma pokrywa dokladnie to, co widac w panelu.
static bool mq_wanted(const char *id_hex, const char *field) {
    char key[DASH_FIELD_LEN];
    snprintf(key, sizeof(key), "%s:%s", id_hex, field);
    return history_is_tracked(key) || nvs_config_dash_field_is_set(key);
}

// Zapamietaj pole i ogloś je raz. Kolejne odczyty tego samego pola nie
// generuja juz ogloszen.
static ha_item_t *ha_remember(const char *id_hex, const char *field, const char *unit,
                              int kind) {
    for (int i = 0; i < s_ha_count; i++)
        if (strcmp(s_ha[i].id_hex, id_hex) == 0 && strcmp(s_ha[i].field, field) == 0) {
            // Rodzaj rozpoznany pozniej niz pole - ogloszenie z poprawna klasa.
            if (kind && s_ha[i].kind != kind) {
                s_ha[i].kind = (uint8_t)kind;
                if (s_connected) ha_announce_one(&s_ha[i]);
            }
            return &s_ha[i];
        }
    ha_item_t *h = NULL;
    if (s_ha_count < HA_MAX) {
        h = &s_ha[s_ha_count++];
    } else {
        // Pelna lista - zajmij miejsce pola odpietego w panelu.
        for (int i = 0; i < s_ha_count && !h; i++)
            if (!mq_wanted(s_ha[i].id_hex, s_ha[i].field)) h = &s_ha[i];
        if (!h) return NULL;
    }
    memset(h, 0, sizeof(*h));
    snprintf(h->id_hex, sizeof(h->id_hex), "%s", id_hex);
    snprintf(h->field,  sizeof(h->field),  "%s", field);
    snprintf(h->unit,   sizeof(h->unit),   "%s", unit ? unit : "");
    h->kind = (uint8_t)kind;
    if (s_connected) ha_announce_one(h);
    return h;
}

static void ha_announce_all(void) {
    for (int i = 0; i < s_ha_count; i++)
        if (mq_wanted(s_ha[i].id_hex, s_ha[i].field)) ha_announce_one(&s_ha[i]);
}

// ---------- beta388: test publikacji w obie strony ----------
// Licznik "wyslanych" liczy wywolania publish() z QoS 0 - sukces oznacza tylko
// zapis do gniazda, bez potwierdzenia od brokera. Broker, ktory po cichu
// odrzuca temat (ACL), i tak go podbija. Test dowodzi wiecej: subskrybuje temat
// testowy, publikuje na nim i czeka, az wiadomosc do nas WROCI.
typedef enum { MT_IDLE = 0, MT_RUN, MT_OK, MT_FAIL } mt_state_t;
#define MT_TIMEOUT_MS 6000

static portMUX_TYPE        s_mt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile mt_state_t s_mt_state = MT_IDLE;
static volatile bool       s_mt_sub_ok = false, s_mt_puback = false;
static volatile int        s_mt_pub_id = -1;
static volatile uint32_t   s_mt_t0 = 0, s_mt_ms_puback = 0, s_mt_ms_echo = 0;
static char                s_mt_topic[TOPIC_MAX];
static char                s_mt_payload[PAYLOAD_MAX];
static const char         *s_mt_err = "";

static uint32_t mt_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Ustaw stan koncowy bez wolania API klienta (bezpieczne z kazdego miejsca).
static void mt_set(mt_state_t st, const char *err) {
    portENTER_CRITICAL(&s_mt_mux);
    s_mt_state = st;
    s_mt_err = err ? err : "";
    portEXIT_CRITICAL(&s_mt_mux);
}

// Zakoncz TRWAJACY test i zdejmij subskrypcje. Wolac POZA sekcja krytyczna -
// unsubscribe to wywolanie API klienta. Zwraca false, gdy test juz sie zakonczyl
// (np. wiadomosc wrocila dokladnie w chwili uplywu czasu) - wtedy nic nie rusza.
static bool mt_finish(mt_state_t st, const char *err) {
    bool bylo = false;
    portENTER_CRITICAL(&s_mt_mux);
    if (s_mt_state == MT_RUN) {
        s_mt_state = st;
        s_mt_err = err ? err : "";
        bylo = true;
    }
    portEXIT_CRITICAL(&s_mt_mux);
    if (bylo && s_client && s_mt_topic[0])
        esp_mqtt_client_unsubscribe(s_client, s_mt_topic);
    return bylo;
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
            // beta388: bez polaczenia subskrypcja przepadla - nie ma czego zdejmowac.
            if (s_mt_state == MT_RUN)
                mt_set(MT_FAIL, "Połączenie z brokerem zerwało się w trakcie testu.");
            break;
        // beta388: kroki testu publikacji. Potwierdzenie subskrypcji dopasowujemy
        // po STANIE testu, nie po msg_id: subskrypcje wysyla zadanie HTTP, a SUBACK
        // obsluguje zadanie MQTT - potrafi przyjsc, zanim zdazymy zapisac jego
        // msg_id. Modul nie subskrybuje niczego innego, wiec to jednoznaczne.
        case MQTT_EVENT_SUBSCRIBED: {
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            if (s_mt_state != MT_RUN || s_mt_sub_ok) break;
            if (ev && ev->error_handle &&
                ev->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                mt_finish(MT_FAIL, "Broker odmówił subskrypcji tematu testowego "
                                   "- najpewniej ACL nie pozwala temu użytkownikowi "
                                   "czytać z tego tematu.");
                break;
            }
            s_mt_sub_ok = true;
            // Publikacja z tego samego zadania co obsluga zdarzen, wiec PUBACK nie
            // moze przyjsc przed zapisaniem msg_id - tu dopasowanie po id jest pewne.
            int id = esp_mqtt_client_publish(s_client, s_mt_topic, s_mt_payload, 0, 1, 0);
            if (id < 0) mt_finish(MT_FAIL, "Moduł nie zdołał wysłać wiadomości testowej.");
            else        s_mt_pub_id = id;
            break;
        }
        case MQTT_EVENT_PUBLISHED: {
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            if (s_mt_state == MT_RUN && ev && ev->msg_id == s_mt_pub_id && !s_mt_puback) {
                s_mt_puback = true;
                s_mt_ms_puback = mt_now_ms() - s_mt_t0;
            }
            break;
        }
        case MQTT_EVENT_DATA: {
            // Powrot wiadomosci dopasowujemy po temacie I tresci - ewentualna
            // wiadomosc zatrzymana (retain) na tym temacie nie udaje sukcesu.
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            if (s_mt_state != MT_RUN || !ev || !ev->topic || !ev->data) break;
            size_t tl = strlen(s_mt_topic), pl = strlen(s_mt_payload);
            if ((size_t)ev->topic_len == tl && memcmp(ev->topic, s_mt_topic, tl) == 0 &&
                (size_t)ev->data_len == pl && memcmp(ev->data, s_mt_payload, pl) == 0) {
                s_mt_ms_echo = mt_now_ms() - s_mt_t0;
                if (mt_finish(MT_OK, ""))
                    ESP_LOGI(TAG, "Test publikacji OK: %s wrocil po %u ms",
                             s_mt_topic, (unsigned)s_mt_ms_echo);
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            mqtt_roll_day();
            s_failed++; s_failed_day++;
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
        mqtt_roll_day();
        if (!s_connected || !s_client) { s_failed++; s_failed_day++; continue; }
        int r = esp_mqtt_client_publish(s_client, it.topic, it.payload,
                                        0, 0, it.retain ? 1 : 0);
        if (r < 0) { s_failed++; s_failed_day++; }
        else       { s_sent++;   s_sent_day++;   }
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
    // beta388: klient zaraz zniknie - trwajacy test konczymy bez unsubscribe.
    if (s_mt_state == MT_RUN)
        mt_set(MT_FAIL, "Klient MQTT został zatrzymany w trakcie testu.");
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

// beta388: start testu publikacji. Wraca od razu; wynik odczytuje sie przez
// mqtt_pub_test_status(). Zwraca false, gdy test nie mogl ruszyc.
bool mqtt_pub_test_start(void) {
    const sih_config_t *c = nvs_config_ptr();

    // Trwajacy test (np. drugi klik) przerywamy porzadnie - ze zdjeciem subskrypcji.
    if (s_mt_state == MT_RUN) mt_finish(MT_FAIL, "Przerwany nowym testem.");

    char pref[24]; prefix_of(pref, sizeof(pref));
    snprintf(s_mt_topic, sizeof(s_mt_topic), "%s/test", pref);
    // Tresc czytelna w nasluchu Home Assistanta: godzina + krotki znacznik.
    // Znacznik odroznia ten test od poprzednich, godzina - od wiadomosci z innego dnia.
    unsigned tok = (unsigned)(esp_timer_get_time() & 0xFFFF);
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tmv; localtime_r(&now, &tmv);
        snprintf(s_mt_payload, sizeof(s_mt_payload), "SIHOS17 test %02d:%02d:%02d #%04x",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tok);
    } else {
        snprintf(s_mt_payload, sizeof(s_mt_payload), "SIHOS17 test #%04x", tok);
    }
    s_mt_sub_ok = false; s_mt_puback = false; s_mt_pub_id = -1;
    s_mt_ms_puback = 0;  s_mt_ms_echo = 0;
    s_mt_t0 = mt_now_ms();

    if (!c->mqtt_enabled) {
        mt_set(MT_FAIL, "MQTT jest wyłączony - zaznacz \u201eWłącz publikację\u201d i zapisz.");
        return false;
    }
    if (!s_client || !s_connected) {
        mt_set(MT_FAIL, "Moduł nie jest połączony z brokerem. Po zapisaniu ustawień "
                        "odczekaj kilka sekund albo sprawdź adres, port i hasło.");
        return false;
    }
    // Stan RUN PRZED wyslaniem subskrypcji - SUBACK moze wrocic natychmiast.
    mt_set(MT_RUN, "");
    if (esp_mqtt_client_subscribe_single(s_client, s_mt_topic, 1) < 0) {
        mt_finish(MT_FAIL, "Moduł nie zdołał wysłać subskrypcji tematu testowego.");
        return false;
    }
    ESP_LOGI(TAG, "Test publikacji: %s", s_mt_topic);
    return true;
}

int mqtt_pub_test_status(char *buf, int cap) {
    // Uplyw czasu oceniamy tu, przy odpytaniu - nie trzeba osobnego timera.
    // Diagnoza zalezy od tego, na ktorym kroku test utknal.
    if (s_mt_state == MT_RUN && (uint32_t)(mt_now_ms() - s_mt_t0) > MT_TIMEOUT_MS) {
        if (!s_mt_sub_ok)
            mt_finish(MT_FAIL, "Broker nie potwierdził subskrypcji w ciągu 6 s.");
        else if (!s_mt_puback)
            mt_finish(MT_FAIL, "Broker nie potwierdził przyjęcia wiadomości w ciągu 6 s.");
        else
            // To jest przypadek, ktorego licznik "wyslanych" nie widzi.
            mt_finish(MT_FAIL, "Broker przyjął wiadomość, ale jej nie rozesłał - najpewniej "
                               "ACL nie pozwala temu użytkownikowi publikować na tym temacie.");
    }
    mt_state_t st = s_mt_state;
    const char *sst = st == MT_RUN ? "run" : st == MT_OK ? "ok" : st == MT_FAIL ? "fail" : "idle";
    const char *step = !s_mt_sub_ok ? "sub" : !s_mt_puback ? "pub" : "echo";
    return snprintf(buf, cap,
        "{\"state\":\"%s\",\"step\":\"%s\",\"topic\":\"%s\",\"payload\":\"%s\","
        "\"puback_ms\":%u,\"ms\":%u,\"error\":\"%s\"}",
        sst, step, s_mt_topic, s_mt_payload,
        (unsigned)s_mt_ms_puback, (unsigned)s_mt_ms_echo, s_mt_err);
}

bool mqtt_pub_field(const char *id_hex, const char *field,
                    double value, const char *unit, int8_t rssi, int kind) {
    if (!s_running || !id_hex || !field) return false;
    if (!mq_wanted(id_hex, field)) return false;
    ha_item_t *h = ha_remember(id_hex, field, unit, kind);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (h) {
        if (h->last_ms && (uint32_t)(now_ms - h->last_ms) < MIN_INTERVAL_MS) return true;
        h->last_ms = now_ms;
    }

    char pref[24]; prefix_of(pref, sizeof(pref));
    char topic[TOPIC_MAX], val[PAYLOAD_MAX];
    snprintf(topic, sizeof(topic), "%s/%s/%s", pref, id_hex, field);
    snprintf(val, sizeof(val), "%.3f", value);
    mq_push(topic, val, true);

    (void)rssi;   // RSSI publikuje mqtt_pub_rssi() raz na ramke
    return true;
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

void mqtt_pub_stats_day(uint32_t *sent, uint32_t *failed) {
    mqtt_roll_day();
    if (sent)   *sent   = s_sent_day;
    if (failed) *failed = s_failed_day;
}
