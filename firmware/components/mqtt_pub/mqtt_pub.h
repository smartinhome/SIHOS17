#pragma once
#include <stdbool.h>
#include <stdint.h>

// Publikowanie odczytow licznikow przez MQTT.
//
// Dziala WYLACZNIE po polaczeniu z siecia domowa - w trybie AP modul nie ma
// dostepu do brokera, wiec klient nie jest wtedy uruchamiany.
//
// Tematy (tylko pola przypiete do dashboardu lub sledzone w historii):
//   <prefiks>/<id licznika>/<pole>   wartosc liczbowa
//   <prefiks>/<id licznika>/rssi     sila sygnalu
//   <prefiks>/status                 online / offline (Last Will)
//
// Przy wlaczonym wykrywaniu Home Assistant kazde pole jest dodatkowo ogloszone
// w homeassistant/sensor/... i tworzy encje z wlasciwa jednostka.

// Start klienta. Wywolywac dopiero po uzyskaniu adresu IP.
void mqtt_pub_start(void);

// Zatrzymanie (np. przed OTA lub przy przejsciu w tryb AP).
void mqtt_pub_stop(void);

// Czy klient jest polaczony z brokerem.
bool mqtt_pub_connected(void);

// Zglos odczyt do wyslania. Wolane z zadania odbioru ramek - NIE blokuje,
// wrzuca do kolejki i wraca. Gdy MQTT jest wylaczony, nic nie robi.
// beta389: wysyla tylko pola z dashboardu lub historii; zwraca true,
// gdy pole do nich nalezy (nawet jesli tym razem pominiete przez odstep).
bool mqtt_pub_field(const char *id_hex, const char *field,
                    double value, const char *unit, int8_t rssi);

// Sila sygnalu - raz na odebrana ramke, nie na kazde pole.
void mqtt_pub_rssi(const char *id_hex, int8_t rssi);

// Zuzycie za zamknieta dobe (publikowane raz na dobe dla sledzonych pol).
void mqtt_pub_day(const char *id_hex, const char *field,
                  const char *date_str, double value, const char *unit);

// Liczba wyslanych wiadomosci i bledow OD STARTU MODULU - do zakladki System.
void mqtt_pub_stats(uint32_t *sent, uint32_t *failed);

// beta388: test publikacji w obie strony (przycisk w zakladce System).
// start  - subskrybuje <prefiks>/test, publikuje tam z QoS 1 i wraca od razu;
// status - JSON ze stanem testu, do odpytywania przez panel.
// Wynik "ok" oznacza, ze wiadomosc WROCILA od brokera - a wiec zostala przyjeta
// i rozeslana. Sam licznik "wyslanych" tego nie dowodzi (patrz mqtt_pub.c).
bool mqtt_pub_test_start(void);
int  mqtt_pub_test_status(char *buf, int cap);

// To samo, ale za BIEZACA DOBE. Liczniki zeruja sie samoczynnie po lokalnej
// polnocy (strefa z konfiguracji), przy pierwszym odwolaniu juz po zmianie daty.
void mqtt_pub_stats_day(uint32_t *sent, uint32_t *failed);
