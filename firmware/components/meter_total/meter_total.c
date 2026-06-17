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
        // CRC sie nie zgadza - zwroc oryginal (bezpieczny fallback)
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

// ---------- Apator: rozmiar rejestru ----------
static int apator_reg_size(uint8_t c) {
    switch (c) {
        case 0x0F: return -1; // marker, obsluga osobno
        case 0x10: return 4;
        case 0x11: return 2;
        case 0x40: return 6; case 0x41: return 2; case 0x42: return 4;
        case 0x43: return 2; case 0x44: return 3;
        case 0x7B: return 49;
        case 0xA0: return 4;
        default:
            if (c >= 0x80 && c <= 0x8F) return 10;
            if (c >= 0xB0 && c <= 0xBF) return 3;
            return -1;
    }
}

// ---------- IZAR LFSR ----------
static const char *IZAR_KEYS[] = { "39BC8A10E66D83F8", "51728910E66D83F8" };

static uint32_t u32be(const uint8_t *d, int o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o+1] << 16) |
           ((uint32_t)d[o+2] << 8) | d[o+3];
}

static bool izar_decode(const uint8_t *frame, int len, double *out_total) {
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
        k = (k ^ u32be(frame, 12));
        int size = len - 17;
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
            dec[i] = frame[i + 17] ^ (kk & 0xFF);
        }
        if (dec[0] == 0x4B) {
            uint32_t t = dec[1] | (dec[2] << 8) | (dec[3] << 16) | ((uint32_t)dec[4] << 24);
            *out_total = t / 1000.0;
            return true;
        }
    }
    return false;
}

// ---------- DIF/VIF: szukaj Volume (m3) lub Energy (kWh) ----------
// Zwraca true gdy znaleziono. Tylko glowny total (pierwszy pasujacy DIF 0x0C/0x04).
static bool difvif_total(const uint8_t *p, int len, double *out_total, int *out_kind) {
    int i = 0;
    while (i < len) {
        uint8_t dif = p[i++];
        if (dif == 0x2F) continue;        // wypelniacz
        if (dif == 0x0F || dif == 0x1F) break; // koniec / producent
        int dn = 0;                        // dlugosc danych wg DIF
        switch (dif & 0x0F) {
            case 0x00: dn = 0; break;
            case 0x01: dn = 1; break;
            case 0x02: dn = 2; break;
            case 0x03: dn = 3; break;
            case 0x04: dn = 4; break;
            case 0x05: dn = 4; break;      // real
            case 0x06: dn = 6; break;
            case 0x07: dn = 8; break;
            case 0x09: dn = 1; break;      // BCD 2 cyfry
            case 0x0A: dn = 2; break;
            case 0x0B: dn = 3; break;
            case 0x0C: dn = 4; break;      // BCD 8 cyfr
            case 0x0E: dn = 6; break;
            default: dn = 0; break;
        }
        // pomin DIFE
        while (i < len && (p[i-1] & 0x80)) i++;
        if (i >= len) break;
        uint8_t vif = p[i++];
        while (i < len && (p[i-1] & 0x80)) i++;  // pomin VIFE
        if (i + dn > len) break;

        // skala wg VIF
        double scale = 0; int kind = 0;
        uint8_t v = vif & 0x7F;
        if (v >= 0x10 && v <= 0x17) {      // Volume m3, 10^(n-6)
            scale = 1.0; int e = (v & 0x07) - 6;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            scale = (e < 0) ? 1.0/m : m; kind = 1;
        } else if (v >= 0x00 && v <= 0x07) { // Energy Wh, 10^(n-3) -> kWh
            int e = (v & 0x07) - 3;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            scale = ((e < 0) ? 1.0/m : m) / 1000.0; kind = 2;
        }

        if (kind != 0) {
            // odczytaj wartosc
            double val = 0;
            if ((dif & 0x0F) == 0x0C || (dif & 0x0F) == 0x09 ||
                (dif & 0x0F) == 0x0A || (dif & 0x0F) == 0x0B || (dif & 0x0F) == 0x0E) {
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

// ---------- Apator woda: AES + rejestry ----------
static bool apator_total(const uint8_t *b, int len, const uint8_t key[16],
                         bool have_key, double *out_total) {
    int ci = -1;
    for (int i = 10; i < 20 && i < len; i++) if (b[i] == 0x7A) { ci = i; break; }
    if (ci < 0) return false;
    uint8_t content[256]; int clen = 0;

    if (have_key) {
        uint8_t iv[16];
        for (int i = 0; i < 8; i++) iv[i] = b[2+i];
        for (int i = 8; i < 16; i++) iv[i] = b[ci+1];
        int encStart = ci + 5;
        int encLen = ((len - encStart) / 16) * 16;
        if (encLen > 0 && encLen <= 240) {
            uint8_t dec[240];
            if (aes_cbc(key, iv, b + encStart, encLen, dec)) {
                if (dec[0] == 0x2F && dec[1] == 0x2F) {
                    clen = encLen - 2;
                    memcpy(content, dec + 2, clen);
                }
            }
        }
    }
    if (clen == 0) {
        int cs = ci + 5;
        if (cs+1 < len && b[cs] == 0x2F && b[cs+1] == 0x2F) cs += 2;
        clen = len - cs;
        if (clen > 256) clen = 256;
        if (clen > 0) memcpy(content, b + cs, clen);
    }
    // rejestry: pomin pierwsze 8 bajtow, szukaj 0x10 (total uint32 LE /1000)
    int i = 8;
    while (i < clen) {
        uint8_t c = content[i];
        if (c == 0xFF) break;
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

// ---------- Amiplus/Unismart: AES + DIF/VIF ----------
static bool difvif_meter(const uint8_t *b, int len, const uint8_t key[16],
                         bool have_key, double *out_total, int *out_kind) {
    if (!have_key) return false;
    int ci = -1;
    for (int i = 10; i < 20 && i < len; i++) if (b[i] == 0x7A) { ci = i; break; }
    if (ci < 0) return false;
    uint8_t iv[16];
    for (int i = 0; i < 8; i++) iv[i] = b[2+i];
    for (int i = 8; i < 16; i++) iv[i] = b[ci+1];
    int encStart = ci + 5;
    int encLen = ((len - encStart) / 16) * 16;
    if (encLen <= 0 || encLen > 240) return false;
    uint8_t dec[240];
    if (!aes_cbc(key, iv, b + encStart, encLen, dec)) return false;
    if (!(dec[0] == 0x2F && dec[1] == 0x2F)) return false;
    return difvif_total(dec + 2, encLen - 2, out_total, out_kind);
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
    while (i < len && nf < max_fields) {
        uint8_t dif = p[i];
        if (dif == 0x2F) { i++; continue; }
        if (dif == 0x0F || dif == 0x1F) break;
        int dn = 0;
        switch (dif & 0x0F) {
            case 0x01: dn=1; break; case 0x02: dn=2; break; case 0x03: dn=3; break;
            case 0x04: dn=4; break; case 0x05: dn=4; break; case 0x06: dn=6; break;
            case 0x07: dn=8; break; case 0x09: dn=1; break; case 0x0A: dn=2; break;
            case 0x0B: dn=3; break; case 0x0C: dn=4; break; case 0x0E: dn=6; break;
            default: dn=0; break;
        }
        int isbcd = ((dif & 0x0F) >= 0x09 && (dif & 0x0F) <= 0x0E && (dif & 0x0F) != 0x0D);
        i++;
        while (i < len && (p[i-1] & 0x80)) i++;
        if (i >= len) break;
        uint8_t vif = p[i]; i++;
        uint8_t vife = 0;
        while (i < len && (p[i-1] & 0x80)) { vife = p[i]; i++; }
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
        bool backflow = (vife == 0x3C);

        if ((v >= 0x00 && v <= 0x07) || vif == 0x83) {
            int e = (v <= 0x07) ? ((v & 0x07) - 3) : 0;
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double kwh = val * ((e < 0) ? 1.0/m : m) / 1000.0;
            if (!backflow) mtf_put(out, &nf, max_fields, "energia_kwh", kwh, "kWh", 1);
            else           mtf_put(out, &nf, max_fields, "produkcja_kwh", kwh, "kWh", 1);
        }
        else if ((v >= 0x28 && v <= 0x2F) || vif == 0xAB || vif == 0xFB) {
            int e = (v >= 0x28 && v <= 0x2F) ? ((v & 0x07) - 3) : 0;  // AB/FB: wartosc w W
            double m = 1; for (int z=0; z<(e<0?-e:e); z++) m *= 10;
            double kw = val * ((e < 0) ? 1.0/m : m) / 1000.0;
            if (!backflow) mtf_put(out, &nf, max_fields, "moc_kw", kw, "kW", 0);
            else           mtf_put(out, &nf, max_fields, "moc_produkcji_kw", kw, "kW", 0);
        }
        else if (vif == 0xFD && (vife >= 0x01 && vife <= 0x03) && volt_idx < 3) {
            char name[24]; snprintf(name, sizeof(name), "napiecie_l%d_v", volt_idx + 1);
            mtf_put(out, &nf, max_fields, name, val / 10.0, "V", 0);
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
    if (strcmp(mf, "APA") == 0 && medium == 0x02 && have_key) {
        int ci = -1;
        for (int i = 10; i < 20 && i < clen; i++) if (clean[i] == 0x7A) { ci = i; break; }
        if (ci < 0) return 0;
        uint8_t iv[16];
        for (int i = 0; i < 8; i++) iv[i] = clean[2+i];
        for (int i = 8; i < 16; i++) iv[i] = clean[ci+1];
        int encStart = ci + 5;
        int encLen = ((clen - encStart) / 16) * 16;
        if (encLen <= 0 || encLen > 240) return 0;
        uint8_t dec[240];
        if (!aes_cbc(key, iv, clean + encStart, encLen, dec)) return 0;
        if (!(dec[0] == 0x2F && dec[1] == 0x2F)) return 0;
        *out_kind = 2;
        return difvif_fields(dec + 2, encLen - 2, out, max_fields);
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

    // IZAR (Diehl SAP, woda) - LFSR, dziala na surowej ramce
    if (strcmp(mf, "SAP") == 0 && medium == 0x01) {
        if (izar_decode(data, (int)len, out_total)) { *out_kind = 1; return true; }  // 1=woda
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
    return false;
}
