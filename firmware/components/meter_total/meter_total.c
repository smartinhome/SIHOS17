#include "meter_total.h"
#include "mbedtls/aes.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------- CRC16 EN 13757 (poly 0x3D65) ----------
static uint16_t crc16(const uint8_t *d, int off, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = d[off + i];
        for (int j = 0; j < 8; j++) {
            if (((crc & 0x8000) >> 8) ^ (b & 0x80)) crc = (crc << 1) ^ 0x3D65;
            else crc = (crc << 1);
            b <<= 1;
        }
    }
    return ~crc & 0xFFFF;
}

// ---------- removeBlockCrc: usuwa CRC blokow formatu A ----------
// Zwraca dlugosc wyniku (zapisuje do out), lub kopiuje 1:1 gdy brak CRC.
// Usuwa CRC blokow bez walidacji (gdy struktura pasuje do mode A z CRC).
static int strip_block_crc(const uint8_t *d, int len, uint8_t *out, int out_cap) {
    int n = 0;
    for (int i = 0; i < 10 && n < out_cap; i++) out[n++] = d[i];
    int pos = 12;
    while (pos + 18 <= len) {
        for (int i = 0; i < 16 && n < out_cap; i++) out[n++] = d[pos + i];
        pos += 18;
    }
    if (pos < len - 2) {
        int dl = (len - 2) - pos;
        for (int i = 0; i < dl && n < out_cap; i++) out[n++] = d[pos + i];
    }
    if (n > 0) out[0] = n - 1;
    return n;
}

static int remove_block_crc(const uint8_t *d, int len, uint8_t *out, int out_cap) {
    // Jesli CI juz na [10] (ramka bez CRC z telegramow testowych) - kopiuj 1:1
    if (len > 10 && (d[10] == 0x7A || d[10] == 0x72)) {
        if (len > out_cap) len = out_cap;
        memcpy(out, d, len);
        return len;
    }
    if (len < 12) { if (len > out_cap) len = out_cap; memcpy(out, d, len); return len; }

    int n = 0;
    // blok 1: 10 bajtow + 2 CRC
    if (crc16(d, 0, 10) != ((d[10] << 8) | d[11])) {
        // CRC naglowka niezgodne. Sprawdz czy to format mode-A z CRC (np. Amiplus CI=0xB1):
        // dlugosc = (L+1) + 2 + nblk*2, a po usunieciu CRC na [10] pojawia sie 0x7A/0x72.
        int L = d[0];
        int nblk = (L + 1 - 10 + 15) / 16;  // ceil
        int exp_len = (L + 1) + 2 + nblk * 2;
        if (len == exp_len) {
            int m = strip_block_crc(d, len, out, out_cap);
            if (m > 10 && (out[10] == 0x7A || out[10] == 0x72)) return m;
        }
        // fallback - zwroc oryginal
        if (len > out_cap) len = out_cap;
        memcpy(out, d, len);
        return len;
    }
    for (int i = 0; i < 10 && n < out_cap; i++) out[n++] = d[i];
    int pos = 12;
    // kolejne bloki: 16 bajtow + 2 CRC
    while (pos + 18 <= len) {
        if (crc16(d, pos, 16) != ((d[pos + 16] << 8) | d[pos + 17])) {
            memcpy(out, d, len > out_cap ? out_cap : len);
            return len > out_cap ? out_cap : len;
        }
        for (int i = 0; i < 16 && n < out_cap; i++) out[n++] = d[pos + i];
        pos += 18;
    }
    // ostatni czesciowy blok
    if (pos < len - 2) {
        int dl = (len - 2) - pos;
        if (crc16(d, pos, dl) != ((d[len - 2] << 8) | d[len - 1])) {
            memcpy(out, d, len > out_cap ? out_cap : len);
            return len > out_cap ? out_cap : len;
        }
        for (int i = 0; i < dl && n < out_cap; i++) out[n++] = d[pos + i];
    }
    if (n > 0) out[0] = n - 1;  // popraw L-field
    return n;
}

// ---------- hex klucza -> 16 bajtow ----------
static bool hex_to_key(const char *hex, uint8_t key[16]) {
    if (!hex || strlen(hex) < 32) return false;
    for (int i = 0; i < 16; i++) {
        char b[3] = { hex[i*2], hex[i*2+1], 0 };
        char *end;
        long v = strtol(b, &end, 16);
        if (*end) return false;
        key[i] = (uint8_t)v;
    }
    return true;
}

// ---------- AES-128-CBC decrypt (mbedtls) ----------
static bool aes_cbc(const uint8_t key[16], const uint8_t iv[16],
                    const uint8_t *in, int len, uint8_t *out) {
    if (len <= 0 || (len % 16) != 0) return false;
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    bool ok = false;
    if (mbedtls_aes_setkey_dec(&ctx, key, 128) == 0) {
        if (mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv_copy,
                                  in, out) == 0) ok = true;
    }
    mbedtls_aes_free(&ctx);
    return ok;
}

// manufacturer z bajtow 2-3 (litery)
static void manuf3(const uint8_t *d, char out[4]) {
    uint16_t m = d[2] | (d[3] << 8);
    out[0] = ((m >> 10) & 0x1F) + 64;
    out[1] = ((m >> 5) & 0x1F) + 64;
    out[2] = (m & 0x1F) + 64;
    out[3] = 0;
}

// ================================================================
//  Warstwa TPL (EN 13757-3/-7): naglowek, tryb szyfrowania, payload
// ================================================================
typedef struct {
    int     ci_pos;      // pozycja bajtu CI
    int     enc_start;   // pierwszy bajt danych (szyfrowanych lub jawnych)
    int     mode;        // tryb bezpieczenstwa (0=jawny, 5=AES-CBC)
    int     nblocks;     // liczba szyfrowanych blokow 16B (0 = nieznana)
    uint8_t acc;         // access number (do IV)
    bool    long_hdr;    // true = CI 0x72 (dlugi naglowek TPL)
} tpl_info_t;

// Znajdz naglowek TPL w ramce bez CRC blokow. CI powinno byc na [10];
// okno wyszukiwania zostawione dla ramek z dodatkowymi warstwami (ELL).
static bool find_tpl(const uint8_t *b, int len, tpl_info_t *t) {
    if (!b || len < 15) return false;
    int ci = -1;
    if (b[10] == 0x7A || b[10] == 0x72) {
        ci = 10;
    } else if (b[10] == 0x8C || b[10] == 0x8D || b[10] == 0x90 || b[10] == 0x91) {
        // Za warstwa ELL/AFL moze stac wlasciwy naglowek TPL. Szukamy go tylko
        // wtedy - wczesniej skanowalismy bajty 10..19 ZAWSZE, wiec przypadkowy
        // bajt 0x7A w tresci ramki (np. IZAR/PRIOS z CI=0xA4) byl brany za
        // naglowek i licznik raportowany jako zaszyfrowany AES.
        for (int i = 11; i < 20 && i < len; i++)
            if (b[i] == 0x7A || b[i] == 0x72) { ci = i; break; }
    }
    if (ci < 0) return false;

    memset(t, 0, sizeof(*t));
    t->ci_pos = ci;
    if (b[ci] == 0x7A) {                       // krotki naglowek: ACC STS CFG(2)
        if (ci + 5 > len) return false;
        t->acc       = b[ci + 1];
        uint16_t cfg = b[ci + 3] | (b[ci + 4] << 8);
        t->mode      = (cfg >> 8) & 0x1F;      // bity 8-12 slowa CFG
        t->nblocks   = (cfg >> 4) & 0x0F;      // liczba szyfrowanych blokow
        t->enc_start = ci + 5;
        t->long_hdr  = false;
    } else {                                   // 0x72, dlugi: ID(4) M(2) V T ACC STS CFG(2)
        if (ci + 13 > len) return false;
        t->acc       = b[ci + 9];
        uint16_t cfg = b[ci + 11] | (b[ci + 12] << 8);
        t->mode      = (cfg >> 8) & 0x1F;
        t->nblocks   = (cfg >> 4) & 0x0F;
        t->enc_start = ci + 13;
        t->long_hdr  = true;
    }
    return true;
}

// Zwraca payload TPL (jawny lub odszyfrowany) do out. Wynik moze zaczynac sie
// markerem 2F2F. Zwraca dlugosc lub -1 (brak naglowka / zly klucz / zly tryb).
// Zgodnie z wmbusmeters: payload zaczynajacy sie 2F2F traktujemy jako juz jawny
// niezaleznie od trybu w CFG (telegramy wczesniej odszyfrowane / testowe).
static int tpl_payload(const uint8_t *b, int len,
                       const uint8_t key[16], bool have_key,
                       uint8_t *out, int out_cap, tpl_info_t *out_tpl) {
    tpl_info_t t;
    if (!find_tpl(b, len, &t)) return -1;
    if (out_tpl) *out_tpl = t;
    int avail = len - t.enc_start;
    if (avail <= 0) return -1;

    // Jawny payload: tryb 0 albo marker 2F2F juz na poczatku
    if (t.mode == 0 ||
        (avail >= 2 && b[t.enc_start] == 0x2F && b[t.enc_start + 1] == 0x2F)) {
        int n = avail;
        if (n > out_cap) n = out_cap;
        memcpy(out, b + t.enc_start, n);
        return n;
    }

    if (t.mode != 5 || !have_key) return -1;   // obslugujemy tylko AES-CBC (tryb 5)

    int encLen = (avail / 16) * 16;
    if (t.nblocks > 0 && t.nblocks * 16 < encLen) encLen = t.nblocks * 16;
    if (encLen <= 0 || encLen > 240) return -1;

    // IV trybu 5: M(2) + A(6) + ACC x8. Dla dlugiego naglowka adres z TPL.
    uint8_t iv[16];
    if (t.long_hdr) {
        int c = t.ci_pos;
        iv[0] = b[c + 5]; iv[1] = b[c + 6];               // M-field z TPL
        for (int i = 0; i < 4; i++) iv[2 + i] = b[c + 1 + i]; // ID
        iv[6] = b[c + 7]; iv[7] = b[c + 8];               // wersja, typ
    } else {
        for (int i = 0; i < 8; i++) iv[i] = b[2 + i];     // M+A z warstwy DLL
    }
    for (int i = 8; i < 16; i++) iv[i] = t.acc;

    uint8_t dec[240];
    if (!aes_cbc(key, iv, b + t.enc_start, encLen, dec)) return -1;
    if (!(dec[0] == 0x2F && dec[1] == 0x2F)) return -1;   // weryfikacja klucza

    int n = encLen;
    if (n > out_cap) n = out_cap;
    memcpy(out, dec, n);
    // dolacz jawna koncowke po szyfrowanych blokach (dozwolone przez norme)
    int trail = avail - encLen;
    if (trail > 0 && n == encLen) {
        int td = trail;
        if (n + td > out_cap) td = out_cap - n;
        if (td > 0) { memcpy(out + n, b + t.enc_start + encLen, td); n += td; }
    }
    return n;
}

// Czy ramka (surowa, z CRC blokow) jest zaszyfrowana i wymaga klucza?
bool meter_total_needs_key(const uint8_t *data, size_t len) {
    if (!data || len < 15) return false;
    uint8_t clean[300];
    int clen = remove_block_crc(data, (int)len, clean, sizeof(clean));
    // Diehl/PRIOS (IZAR, Hydrometer) NIE uzywaja AES - maja wlasne maskowanie
    // LFSR z kluczami wbudowanymi w firmware. Nigdy nie prosimy o klucz.
    if (clen >= 4) {
        char mf[4]; manuf3(clean, mf);
        if (strcmp(mf, "SAP") == 0 || strcmp(mf, "DME") == 0 || strcmp(mf, "HYD") == 0)
            return false;
    }
    tpl_info_t t;
    if (!find_tpl(clean, clen, &t)) return false;
    if (t.mode == 0) return false;
    int avail = clen - t.enc_start;
    if (avail >= 2 && clean[t.enc_start] == 0x2F && clean[t.enc_start + 1] == 0x2F)
        return false;   // juz jawny
    return true;
}

// ---------- Apator: rozmiar rejestru ----------
// Pelna tabela wg gramatyki wmbusmeters drivers/src/apator162.xmq.
// Zwraca liczbe bajtow danych po bajcie rejestru, lub -1 gdy nieznany.
static int apator_reg_size(uint8_t c) {
    switch (c) {
        case 0x00: return 4;                  // data
        case 0x01: return 3;                  // usterki
        case 0x0F: return -1;                 // marker, obsluga osobno
        case 0x10: return 4;                  // TOTAL (uint32 LE, litry)
        case 0x11: return 2;                  // przeplyw
        case 0x40: return 6;
        case 0x41: return 2;
        case 0x42: return 4;
        case 0x43: return 2;
        case 0x44: return 3;
        case 0xA0: case 0xA1: case 0xA4: case 0xF0: return 4;
        case 0xA2: case 0xA5: case 0xA9: case 0xAF: return 1;
        case 0xA3: return 7;
        case 0xA6: return 3;
        case 0xA7: case 0xA8: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
            return 2;
        case 0x8A: return 9;                  // quad+quad+byte
        case 0x8B: case 0x8C: return 6;       // triplet+triplet
        case 0x8E: return 7;                  // quad+triplet
        case 0xB0: return 5;
        case 0xB1: case 0xB3: return 8;
        case 0xB2: case 0xB5: return 16;
        case 0xB4: return 2;
        default:
            // 0x71..0x7C: byte + (c - 0x6F) quadow = 1 + 4*(c-0x6F)
            if (c >= 0x71 && c <= 0x7C) return 1 + 4 * (c - 0x6F);
            // 0x80-0x84, 0x86, 0x87: quad+quad+word = 10
            if (c == 0x80 || c == 0x81 || c == 0x82 || c == 0x83 ||
                c == 0x84 || c == 0x86 || c == 0x87) return 10;
            // 0x85, 0x88, 0x8F: quad+quad+triplet = 11
            if (c == 0x85 || c == 0x88 || c == 0x8F) return 11;
            if (c >= 0xB6 && c <= 0xBF) return 3;
            if (c >= 0xC0 && c <= 0xC7) return 3;
            if (c == 0xD0 || c == 0xD3) return 3;
            return -1;
    }
}

// ---------- IZAR LFSR ----------
static const char *IZAR_KEYS[] = { "39BC8A10E66D83F8", "51728910E66D83F8" };

static uint32_t u32be(const uint8_t *d, int o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o+1] << 16) |
           ((uint32_t)d[o+2] << 8) | d[o+3];
}

// Dekoder Diehl PRIOS. frame = ramka BEZ CRC blokow (CI na [10]).
// Offsety zgodne z wmbusmeters manufacturer_specificities.cc:
// klucz XOR bajty [2..5], [6..9], [10..13]; payload od [15].
static bool izar_decode(const uint8_t *frame, int len, double *out_total) {
    if (len < 20) return false;
    for (int ki = 0; ki < 2; ki++) {
        uint8_t kb[8];
        const char *kh = IZAR_KEYS[ki];
        bool bad = false;
        for (int i = 0; i < 8; i++) {
            char b[3] = { kh[i*2], kh[i*2+1], 0 };
            char *end; long v = strtol(b, &end, 16);
            if (*end) { bad = true; break; }
            kb[i] = (uint8_t)v;
        }
        if (bad) continue;
        uint32_t k = u32be(kb, 0) ^ u32be(kb, 4);
        k = (k ^ u32be(frame, 2));
        k = (k ^ u32be(frame, 6));
        k = (k ^ u32be(frame, 10));
        int size = len - 15;
        if (size <= 0) continue;
        uint8_t dec[64];
        if (size > 64) size = 64;
        uint32_t kk = k;
        for (int i = 0; i < size; i++) {
            for (int j = 0; j < 8; j++) {
                uint32_t bit = (((kk >> 1) & 1) ^ ((kk >> 2) & 1) ^
                                ((kk >> 11) & 1) ^ ((kk >> 31) & 1)) & 1;
                kk = (kk << 1) | bit;
            }
            dec[i] = frame[i + 15] ^ (kk & 0xFF);
            if (i == 0 && dec[0] != 0x4B) break;   // bajt kontrolny PRIOS
        }
        if (dec[0] == 0x4B) {
            uint32_t t = dec[1] | (dec[2] << 8) | (dec[3] << 16) | ((uint32_t)dec[4] << 24);
            *out_total = t / 1000.0;
            return true;
        }
    }
    return false;
}

// ---------- pomocnik DIF/VIF: dlugosc danych wg dolnego nibbla DIF ----------
static int dif_data_len(uint8_t dif) {
    switch (dif & 0x0F) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0x02: return 2;
        case 0x03: return 3;
        case 0x04: return 4;
        case 0x05: return 4;   // real 32b
        case 0x06: return 6;
        case 0x07: return 8;
        case 0x08: return 0;   // selection for readout
        case 0x09: return 1;   // BCD2
        case 0x0A: return 2;   // BCD4
        case 0x0B: return 3;   // BCD6
        case 0x0C: return 4;   // BCD8
        case 0x0D: return -1;  // LVAR - dlugosc w pierwszym bajcie danych
        case 0x0E: return 6;   // BCD12
        default:   return 0;   // 0x0F obslugiwane wyzej jako koniec
    }
}

// Dlugosc rekordu LVAR (DIF 0x0D) na podstawie bajtu LVAR (EN 13757-3).
// Zwracana wartosc NIE obejmuje samego bajtu LVAR.
static int lvar_data_len(uint8_t lvar) {
    if (lvar <= 0xBF) return lvar;             // ASCII
    if (lvar <= 0xCF) return lvar - 0xC0;      // BCD dodatnie
    if (lvar <= 0xDF) return lvar - 0xD0;      // BCD ujemne
    if (lvar <= 0xEF) return lvar - 0xE0;      // binarne
    if (lvar == 0xF8) return 8;                // float64 wg rozszerzen
    return -1;                                 // niezdefiniowane - przerwij parsowanie
}

// ---------- DIF/VIF: szukaj glownego total - Volume (m3) lub Energy (kWh) ----------
// Akceptuje tylko rekord chwilowy, biezacy (funkcja=0, storage=0, bez taryfy).
static bool difvif_total(const uint8_t *p, int len, double *out_total, int *out_kind) {
    int i = 0;
    while (i < len) {
        uint8_t dif = p[i++];
        if (dif == 0x2F) continue;             // wypelniacz
        if (dif == 0x0F || dif == 0x1F) break; // dane producenta / koniec
        int dn = dif_data_len(dif);
        // funkcja (bity 4-5): 0=wartosc chwilowa; storage LSB (bit 6)
        bool current = ((dif & 0x70) == 0);
        // pomin DIFE; taryfa != 0 dyskwalifikuje rekord jako glowny total
        while (i < len && (p[i-1] & 0x80)) {
            if (((p[i] >> 4) & 0x03) != 0 || (p[i] & 0x0F) != 0) current = false;
            i++;
        }
        if (i >= len) break;
        uint8_t vif = p[i++];
        while (i < len && (p[i-1] & 0x80)) i++;  // pomin VIFE
        if (dn < 0) {                            // LVAR
            if (i >= len) break;
            dn = lvar_data_len(p[i]);
            if (dn < 0) break;
            i++;                                 // bajt LVAR
        }
        if (i + dn > len) break;

        // skala wg VIF
        double scale = 0; int kind = 0;
        uint8_t v = vif & 0x7F;
        if (v >= 0x10 && v <= 0x17) {      // Volume m3, 10^(n-6)
            int e = (v & 0x07) - 6;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            scale = (e < 0) ? 1.0/m : m; kind = 1;
        } else if (v <= 0x07) {            // Energy Wh, 10^(n-3) -> kWh
            int e = (v & 0x07) - 3;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            scale = ((e < 0) ? 1.0/m : m) / 1000.0; kind = 2;
        }

        if (kind != 0 && current && dn > 0) {
            // odczytaj wartosc
            double val = 0;
            int lo = dif & 0x0F;
            if (lo >= 0x09 && lo <= 0x0E && lo != 0x0D) {
                // BCD
                double mult = 1;
                for (int b = 0; b < dn; b++) {
                    val += (p[i+b] & 0x0F) * mult; mult *= 10;
                    val += ((p[i+b] >> 4) & 0x0F) * mult; mult *= 10;
                }
            } else {
                // integer LE
                int64_t iv = 0;
                for (int b = 0; b < dn; b++) iv |= ((int64_t)p[i+b]) << (8*b);
                val = (double)iv;
            }
            *out_total = val * scale;
            *out_kind = kind;
            return true;
        }
        i += dn;
    }
    return false;
}

// ---------- Apator woda: TPL/AES + rejestry ----------
static bool apator_total(const uint8_t *b, int len, const uint8_t key[16],
                         bool have_key, double *out_total) {
    uint8_t payload[256];
    int plen = tpl_payload(b, len, key, have_key, payload, sizeof(payload), NULL);
    if (plen <= 0) return false;

    const uint8_t *content = payload;
    int clen = plen;
    if (clen >= 2 && content[0] == 0x2F && content[1] == 0x2F) { content += 2; clen -= 2; }

    // struktura (wmbusmeters apator162): 1 bajt wiodacy (0x0F lub 0x80),
    // 7 bajtow statusu, potem rejestry; 0x10 = total uint32 LE w litrach
    int i = 8;
    while (i < clen) {
        uint8_t c = content[i];
        if (c == 0xFF) break;                 // wypelnienie przed suma kontrolna
        int sz = apator_reg_size(c);
        i++;
        if (sz == -1 || i + sz > clen) break;
        if (c == 0x10 && sz == 4) {
            uint32_t t = content[i] | (content[i+1]<<8) | (content[i+2]<<16) | ((uint32_t)content[i+3]<<24);
            *out_total = t / 1000.0;
            return true;
        }
        i += sz;
    }
    return false;
}

// ---------- Liczniki DIF/VIF (Amiplus/Unismart/generyczne): TPL/AES + total ----------
static bool difvif_meter(const uint8_t *b, int len, const uint8_t key[16],
                         bool have_key, double *out_total, int *out_kind) {
    uint8_t payload[256];
    int plen = tpl_payload(b, len, key, have_key, payload, sizeof(payload), NULL);
    if (plen <= 0) return false;
    const uint8_t *p = payload;
    if (plen >= 2 && p[0] == 0x2F && p[1] == 0x2F) { p += 2; plen -= 2; }
    return difvif_total(p, plen, out_total, out_kind);
}

// ---------- Glowna funkcja ----------
// Pomocnik: znajdz pole po nazwie (do nadpisania zera niezerowa wartoscia)
static int mtf_find(mtf_field_t *out, int nf, const char *name) {
    for (int k = 0; k < nf; k++) if (strcmp(out[k].field, name) == 0) return k;
    return -1;
}
static void mtf_put(mtf_field_t *out, int *nf, int max, const char *name,
                    double val, const char *unit, int cumulative) {
    int idx = mtf_find(out, *nf, name);
    if (idx >= 0) {
        // pole juz istnieje - nadpisz tylko gdy nowa wartosc niezerowa a stara zerowa
        if (out[idx].value == 0 && val != 0) out[idx].value = val;
        return;
    }
    if (*nf >= max) return;
    snprintf(out[*nf].field, sizeof(out[*nf].field), "%s", name);
    out[*nf].value = val;
    snprintf(out[*nf].unit, sizeof(out[*nf].unit), "%s", unit);
    out[*nf].cumulative = cumulative;
    (*nf)++;
}

// ---------- Wielopolowy parser DIF/VIF (Amiplus: energia/moc/napiecia) ----------
static int difvif_fields(const uint8_t *p, int len, mtf_field_t *out, int max_fields) {
    int nf = 0;
    int i = 0;
    int volt_idx = 0;
    int curr_idx = 0;
    while (i < len && nf < max_fields) {
        uint8_t dif = p[i];
        if (dif == 0x2F) { i++; continue; }
        if (dif == 0x0F || dif == 0x1F) break;
        int dn = dif_data_len(dif);
        int isbcd = ((dif & 0x0F) >= 0x09 && (dif & 0x0F) <= 0x0E && (dif & 0x0F) != 0x0D);
        int func = (dif >> 4) & 0x03;      // 0=chwilowa, 1=max, 2=min, 3=blad
        bool storage = (dif & 0x40) != 0;  // wartosc z poprzedniego okresu
        i++;
        // odczyt DIFE - wyciagnij numer taryfy z bitow 4-5 pierwszego DIFE
        int tariff = 0;
        bool first_dife = true;
        while (i < len && (p[i-1] & 0x80)) {
            if (first_dife) { tariff = (p[i] >> 4) & 0x03; first_dife = false; }
            i++;
        }
        if (i >= len) break;
        uint8_t vif = p[i]; i++;
        uint8_t vife_first = 0, vife = 0;
        bool have_vife = false;
        while (i < len && (p[i-1] & 0x80)) {
            vife = p[i];
            if (!have_vife) { vife_first = vife; have_vife = true; }
            i++;
        }
        if (dn < 0) {                      // LVAR
            if (i >= len) break;
            dn = lvar_data_len(p[i]);
            if (dn < 0) break;
            i++;
        }
        if (i + dn > len) break;

        double val = 0;
        if (isbcd) {
            double mult = 1;
            for (int b = 0; b < dn; b++) {
                val += (p[i+b] & 0x0F) * mult; mult *= 10;
                val += ((p[i+b] >> 4) & 0x0F) * mult; mult *= 10;
            }
        } else {
            int64_t iv = 0;
            for (int b = 0; b < dn; b++) iv |= ((int64_t)p[i+b]) << (8*b);
            val = (double)iv;
        }

        uint8_t v = vif & 0x7F;
        // "energia wsteczna" (produkcja): VIFE 0x3C w lancuchu (Amiplus: 83 3C)
        bool backflow = (vife == 0x3C || vife_first == 0x3C);

        if (v <= 0x07 && func == 0 && !storage) {
            // Energia Wh, 10^(n-3) -> kWh
            int e = (v & 0x07) - 3;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double kwh = val * ((e < 0) ? 1.0/m : m) / 1000.0;
            if (tariff == 0) {
                // suma (bez taryfy)
                if (!backflow) mtf_put(out, &nf, max_fields, "energia_kwh", kwh, "kWh", 1);
                else           mtf_put(out, &nf, max_fields, "produkcja_kwh", kwh, "kWh", 1);
            } else {
                // energia/produkcja w konkretnej taryfie (G12: t1=dzien, t2=noc)
                char name[24];
                if (!backflow) snprintf(name, sizeof(name), "energia_t%d_kwh", tariff);
                else           snprintf(name, sizeof(name), "produkcja_t%d_kwh", tariff);
                mtf_put(out, &nf, max_fields, name, kwh, "kWh", 1);
            }
        }
        else if (v >= 0x28 && v <= 0x2F && !storage) {
            // Moc W, 10^(n-3) -> kW; funkcja 1 = wartosc maksymalna
            int e = (v & 0x07) - 3;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double kw = val * ((e < 0) ? 1.0/m : m) / 1000.0;
            if (func == 0) {
                if (!backflow) mtf_put(out, &nf, max_fields, "moc_kw", kw, "kW", 0);
                else           mtf_put(out, &nf, max_fields, "moc_produkcji_kw", kw, "kW", 0);
            } else if (func == 1 && !backflow) {
                mtf_put(out, &nf, max_fields, "moc_max_kw", kw, "kW", 0);
            }
        }
        else if (vif == 0xFD && (vife_first & 0x70) == 0x50 &&
                 func == 0 && !storage && curr_idx < 3) {
            // Prad fazowy: VIF FD, VIFE 0x50-0x5F -> 10^(nnnn-12) A,
            // ostatni VIFE (po 0xFC "at phase") = numer fazy 1-3
            int e = (vife_first & 0x0F) - 12;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double amps = val * ((e < 0) ? 1.0/m : m);
            int phase = (vife >= 1 && vife <= 3) ? (int)vife : (curr_idx + 1);
            char name[24]; snprintf(name, sizeof(name), "prad_l%d_a", phase);
            mtf_put(out, &nf, max_fields, name, amps, "A", 0);
            curr_idx++;
        }
        else if (vif == 0xFB && func == 0 && !storage) {
            // Moc bierna: VIFE 0x14-0x15 -> 10^(n) VAR
            // Energia bierna: VIFE 0x02-0x03 -> 10^(n) kVARh
            // Trailing VIFE 0x3C rozroznia pojemnosciowa (C) od indukcyjnej (L).
            int v = vife_first & 0x7F;
            if (v >= 0x14 && v <= 0x15) {
                double m = (v & 1) ? 10.0 : 1.0;
                char name[24]; snprintf(name, sizeof(name), "moc_bierna_%c_var",
                                        backflow ? 'c' : 'l');
                mtf_put(out, &nf, max_fields, name, val * m, "VAR", 0);
            } else if (v >= 0x02 && v <= 0x03) {
                double m = (v & 1) ? 10.0 : 1.0;
                char name[32]; snprintf(name, sizeof(name), "energia_bierna_%c_kvarh",
                                        backflow ? 'c' : 'l');
                mtf_put(out, &nf, max_fields, name, val * m, "kVARh", 1);
            }
        }
        else if (vif == 0xFD && (vife_first & 0x70) == 0x40 &&
                 func == 0 && !storage && volt_idx < 3) {
            // Napiecie: VIF FD, VIFE 0x40-0x4F -> 10^(nnnn-9) V,
            // ostatni VIFE (po 0xFC "at phase") = numer fazy 1-3
            int e = (vife_first & 0x0F) - 9;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double volts = val * ((e < 0) ? 1.0/m : m);
            int phase = (vife >= 1 && vife <= 3) ? (int)vife : (volt_idx + 1);
            char name[24]; snprintf(name, sizeof(name), "napiecie_l%d_v", phase);
            mtf_put(out, &nf, max_fields, name, volts, "V", 0);
            volt_idx++;
        }
        i += dn;
    }
    return nf;
}

int meter_total_extract_fields(const uint8_t *data, size_t len,
                               const char *key_hex,
                               mtf_field_t *out, int max_fields, int *out_kind) {
    if (!data || len < 12 || !out || max_fields < 1) return 0;
    *out_kind = 0;
    uint8_t key[16];
    bool have_key = hex_to_key(key_hex, key);
    char mf[4]; manuf3(data, mf);
    uint8_t medium = data[9];
    uint8_t clean[300];
    int clen = remove_block_crc(data, (int)len, clean, sizeof(clean));

    // Tylko Amiplus prad (APA, medium 0x02) ma wiele pol
    if (strcmp(mf, "APA") == 0 && medium == 0x02) {
        uint8_t payload[256];
        int plen = tpl_payload(clean, clen, key, have_key, payload, sizeof(payload), NULL);
        if (plen > 0) {
            const uint8_t *p = payload;
            if (plen >= 2 && p[0] == 0x2F && p[1] == 0x2F) { p += 2; plen -= 2; }
            int nf = difvif_fields(p, plen, out, max_fields);
            if (nf > 0) { *out_kind = 2; return nf; }
        }
        return 0;
    }

    // Pozostale liczniki: jedno pole total (uzyj istniejacej funkcji)
    double total = 0; int kind = 0;
    if (meter_total_extract(data, len, key_hex, &total, &kind)) {
        *out_kind = kind;
        snprintf(out[0].field, sizeof(out[0].field), "%s", kind == 2 ? "energia_kwh" : "total_m3");
        out[0].value = total;
        snprintf(out[0].unit, sizeof(out[0].unit), "%s", kind == 2 ? "kWh" : "m3");
        out[0].cumulative = 1;
        return 1;
    }
    return 0;
}

bool meter_total_extract(const uint8_t *data, size_t len,
                         const char *key_hex,
                         double *out_total, int *out_kind) {
    if (!data || len < 12) return false;
    *out_total = 0; *out_kind = 0;

    uint8_t key[16];
    bool have_key = hex_to_key(key_hex, key);

    char mf[4];
    manuf3(data, mf);
    uint8_t medium = data[9];

    // usun CRC blokow do bufora roboczego
    uint8_t clean[300];
    int clen = remove_block_crc(data, (int)len, clean, sizeof(clean));

    // Techem MK Radio 4 (TCH, wersja 0x95, CI 0xA2 = format producenta).
    // Nie ma tu DIF/VIF - dane leza na stalych pozycjach ramki BEZ CRC blokow:
    //   [14..15] licznik z konca poprzedniego okresu rozliczeniowego (LE, 0.1 m3)
    //   [18..19] przyrost od tamtego momentu               (LE, 0.1 m3)
    // Stan biezacy = suma obu. Zgodne z wmbusmeters (driver mkradio4).
    // Typ 0x62 = woda ciepla, 0x72 = woda zimna - oba obslugiwane tak samo.
    if (strcmp(mf, "TCH") == 0 && clen >= 20 && clean[8] == 0x95 &&
        (clean[9] == 0x62 || clean[9] == 0x72) && clean[10] == 0xA2) {
        uint16_t prev = (uint16_t)(clean[14] | (clean[15] << 8));
        uint16_t curr = (uint16_t)(clean[18] | (clean[19] << 8));
        *out_total = (double)(prev + curr) / 10.0;
        *out_kind  = 1;   // woda
        return true;
    }
    // IZAR / PRIOS (Diehl: SAP, DME, Hydrometer: HYD) - LFSR, bez klucza AES.
    // Walidacja bajtem kontrolnym 0x4B, wiec brak filtra medium.
    if (strcmp(mf, "SAP") == 0 || strcmp(mf, "DME") == 0 || strcmp(mf, "HYD") == 0) {
        if (izar_decode(clean, clen, out_total)) { *out_kind = 1; return true; }  // 1=woda
        return false;
    }
    // Apator woda (APA, medium 0x06/0x07)
    if (strcmp(mf, "APA") == 0 && (medium == 0x07 || medium == 0x06)) {
        if (apator_total(clean, clen, key, have_key, out_total)) { *out_kind = 1; return true; }  // 1=woda
        return false;
    }
    // Amiplus prad (APA, medium 0x02) - AES + DIF/VIF
    if (strcmp(mf, "APA") == 0 && medium == 0x02) {
        int k = 0;
        if (difvif_meter(clean, clen, key, have_key, out_total, &k)) { *out_kind = 2; return true; }  // 2=prad
        return false;
    }
    // Unismart gaz (AMX, medium 0x03)
    if (strcmp(mf, "AMX") == 0 && medium == 0x03) {
        int k = 0;
        if (difvif_meter(clean, clen, key, have_key, out_total, &k)) { *out_kind = 3; return true; }  // 3=gaz
        return false;
    }
    // Generyczny fallback: liczniki z naglowkiem TPL (0x7A/0x72) i standardowym
    // DIF/VIF (jawne lub AES-CBC z kluczem) - np. iPerl i inne EN 13757-3.
    {
        int k = 0;
        if (difvif_meter(clean, clen, key, have_key, out_total, &k)) {
            if (k == 1 && medium == 0x03) k = 3;  // objetosc na liczniku gazu
            *out_kind = k;
            return true;
        }
    }
    return false;
}
