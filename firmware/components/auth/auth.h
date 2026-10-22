#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// beta404: logowanie do panelu (domyslnie wylaczone).
//
// Haslo trzymane w NVS jako PBKDF2-HMAC-SHA256 z losowa sola - nie da sie go
// odczytac. Po zalogowaniu przegladarka dostaje losowy klucz sesji w ciasteczku
// HttpOnly (skrypty strony go nie widza). Kazde zapytanie /api/* przechodzi
// przez auth_gate(); bez logowania dziala tylko strona panelu (ekran logowania)
// i /api/auth. MQTT, API ESPHome i e-ink nie korzystaja z panelu - bez zmian.

// Wczytuje ustawienia i zapamietane sesje z NVS (po nvs_config_init).
void auth_init(void);

// Czy logowanie jest wlaczone.
bool auth_enabled(void);

// Bramka dla handlerow API: req->user_ctx = oryginalny handler.
// Przy wylaczonym logowaniu albo waznej sesji wola handler, inaczej 401.
esp_err_t auth_gate(httpd_req_t *req);

// GET /api/auth - stan logowania; POST /api/auth - login, wylogowanie, ustawienia.
esp_err_t auth_handle(httpd_req_t *req);

// Przycisk BOOT trzymany 10 s: wylacza logowanie i kasuje sesje.
// Pozostale ustawienia modulu zostaja. Wolane z taska przycisku.
void auth_button_reset(void);

#ifdef __cplusplus
}
#endif
