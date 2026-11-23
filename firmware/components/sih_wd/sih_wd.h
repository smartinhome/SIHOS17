#pragma once
#include <stdbool.h>

// beta436: straznik zawieszen calego modulu. Modul potrafil stanac w calosci
// (WWW, MQTT, e-ink) i wracal dopiero po odcieciu zasilania, a watchdog zadan
// w tej konfiguracji tylko ostrzega. Straznik pilnuje dwoch rzeczy:
//  - zadania obrobki ramek (zglasza sie co najwyzej co 5 s, takze bez ramek),
//  - biezacego zapytania WWW (kazde przechodzi przez auth_gate).
// Gdy ktores stoi za dlugo, zapisuje w pamieci RTC co i na jakim etapie stalo,
// a potem restartuje modul. Po starcie ten wpis trafia do logu.

void sih_wd_init(void);                       // raport z poprzedniego restartu + start
void sih_wd_worker_alive(void);               // zadanie ramek: zyje
void sih_wd_worker_stage(const char *stage);  // literal; NULL = czeka na ramke
void sih_wd_http_enter(const char *uri);
void sih_wd_http_leave(void);
