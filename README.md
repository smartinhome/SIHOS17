# SIH wMbus Reader

Własny firmware ESP32-C6 dla modułu SIH wMbus Reader 868MHz.  
Odbiera ramki wMbus (T1/C1) przez CC1101, dekoduje liczniki i udostępnia
dane przez Web UI oraz REST API.

## Sprzęt

| Komponent | Opis |
|-----------|------|
| ESP32-C6 | esp32-c6-devkitc-1, 8MB flash |
| CC1101 | Radio 868 MHz, SPI |
| Waveshare 2.13" v3 | E-ink, SPI (Faza 2) |
| WS2812 | LED RGB status |

## Pinout

| Sygnał | GPIO |
|--------|------|
| CC1101 CLK | 6 |
| CC1101 MOSI | 7 |
| CC1101 MISO | 2 |
| CC1101 CS | 10 |
| CC1101 GDO0 | 5 |
| CC1101 GDO2 | 3 |
| E-ink CS | 11 |
| E-ink DC | 21 |
| E-ink BUSY | 20 |
| E-ink RST | 22 |
| LED RGB | 8 |
| Przycisk BOOT | 9 |
| LED wMbus | 19 |

## Pierwsze uruchomienie

```bash
# Wymagania: ESP-IDF v5.2+
git clone https://github.com/TWOJ_NICK/sih-wmbus-reader
cd sih-wmbus-reader/firmware

idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Po uruchomieniu urządzenie tworzy sieć AP:
- **SSID:** `SIH-wMbus`
- **Hasło:** `smartinhome`
- **Web UI:** http://192.168.4.1

Skonfiguruj WiFi w Web UI → Konfiguracja → Sieć WiFi.

## Web UI

| Zakładka | Opis |
|----------|------|
| Dashboard | Aktualne odczyty wszystkich liczników |
| Liczniki | Szczegółowy widok każdego licznika |
| **Złap licznik** | Podgląd surowych ramek z eteru na żywo, dekodowanie w przeglądarce, wpisanie klucza AES gdy wymagany, zapis licznika |
| Konfiguracja | WiFi, radio, lista liczników z kluczami |
| Aktualizacja | OTA z pliku lub GitHub Releases |

## REST API

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/api/status` | GET | Status urządzenia |
| `/api/meters` | GET | Odczyty wszystkich liczników |
| `/api/frames` | GET | Ostatnie surowe ramki wMbus (hex) — do dekodowania w Web UI |
| `/api/config` | GET | Bieżąca konfiguracja |
| `/api/config/wifi` | POST | Zmień WiFi |
| `/api/config/meter` | POST | Dodaj/zaktualizuj licznik z kluczem AES (`{id,type,key,name}`) |
| `/api/ota/url` | POST | OTA z URL |
| `/api/ota/upload` | POST | OTA upload .bin |
| `/api/ota/status` | GET | Postęp OTA |
| `/api/restart` | POST | Restart |

## OTA z GitHub Releases

Każdy push taga `vX.Y.Z` uruchamia GitHub Actions który:
1. Buduje firmware przez ESP-IDF w kontenerze
2. Tworzy Release z plikami `.bin`
3. Generuje changelog z commitów

URL do OTA:
```
https://github.com/TWOJ_NICK/sih-wmbus-reader/releases/latest/download/firmware.bin
```

## Dekoder wMbus

Firmware odbiera ramki przez CC1101 i **buforuje je w surowej postaci**
(`/api/frames`). Samo **dekodowanie odbywa się w Web UI** (`webui/wmbus-decoder.js`),
gdzie logika z `bodek85/esphome-components` (oparta o `wmbusmeters`) została
przeniesiona do czystego JavaScript:

- parsowanie warstwy łącza (producent, ID, wersja, medium, CI),
- wykrywanie trybu zabezpieczeń (otwarty / AES-128 tryb 5 / Diehl PRIOS),
- **AES-128 CBC w czystym JS** (ESP32 serwuje po HTTP, więc WebCrypto nie jest
  dostępne) — weryfikowane testem FIPS-197,
- **deobfuskacja Diehl LFSR** dla liczników `izar`/PRIOS (NIE wymaga klucza),
- generyczny parser rekordów DIF/VIF (EN 13757-3).

Poprawność jest weryfikowana na realnych wektorach z `wmbusmeters`:

```bash
node webui/test-decoder.js   # izar ×6, iperl (DIF/VIF), AES KAT — 25/25 pass
```

Obsługiwane typy liczników:
- `izar` — woda (Diehl PRIOS, LFSR, bez klucza) — **zweryfikowane**
- liczniki otwarte (tryb 0) i AES-128 tryb 5 — generyczny DIF/VIF
- `amiplus` — elektryczność (OTUS3), `apator162` — woda, `unismart` — gaz
  (mapowanie typu + ścieżka generyczna/AES; sterowniki specyficzne w planach)

## Struktura projektu

```
firmware/
├── main/               # Punkt wejścia, definicje pinów
├── components/
│   ├── cc1101/         # Sterownik radia SPI
│   ├── wmbus_decoder/  # Dekoder ramek + integracja bodek85
│   ├── wifi_manager/   # WiFi STA + fallback AP
│   ├── webserver/      # HTTP server + REST API
│   ├── ota_manager/    # OTA z URL i z bufora
│   └── nvs_config/     # Konfiguracja w NVS flash
webui/
├── index.html          # Single-page Web UI (zakładka "Złap licznik")
├── wmbus-decoder.js    # Dekoder wMbus w JS (link-layer + AES + Diehl-LFSR + DIF/VIF)
└── test-decoder.js     # Testy dekodera na realnych wektorach (node)
.github/workflows/
└── build.yml           # CI/CD: build + release
```

## Roadmap

- [x] Faza 1 — Radio + Web UI + OTA + REST API
- [ ] Faza 2 — E-ink (6 stron jak w ESPHome)
- [ ] Faza 3 — Historia LittleFS (dzienna/miesięczna/roczna)
- [ ] Faza 4 — Integracja wmbusmeters (oficjalny)
- [ ] Faza 5 — MQTT

## Licencja

MIT
