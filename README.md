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
| Konfiguracja | WiFi, radio, lista liczników z kluczami |
| Aktualizacja | OTA z pliku lub GitHub Releases |

## REST API

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/api/status` | GET | Status urządzenia |
| `/api/meters` | GET | Odczyty wszystkich liczników |
| `/api/config` | GET | Bieżąca konfiguracja |
| `/api/config/wifi` | POST | Zmień WiFi |
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

Używa komponentu `bodek85/esphome-components` (warstwy dekodowania)
portowanego jako natywny komponent ESP-IDF.

Obsługiwane typy liczników:
- `amiplus` — elektryczność (OTUS3)
- `izar` — woda
- `apator162` — woda
- `unismart` — gaz

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
└── index.html          # Single-page Web UI
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
