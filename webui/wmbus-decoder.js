/*
 * wmbus-decoder.js — dekoder ramek wM-Bus po stronie przeglądarki.
 *
 * Logika przeniesiona z bodek85/esphome-components (oparte o wmbusmeters):
 *   - parsowanie warstwy łącza (L/C/M/A, producent, ID, wersja, medium, CI)
 *   - wykrywanie trybu zabezpieczeń (otwarty / AES-128 tryb 5 / Diehl PRIOS)
 *   - deszyfracja AES-128 CBC (tryb 5) — czysty JS, bo ESP32 serwuje po HTTP
 *     (SubtleCrypto/WebCrypto wymaga bezpiecznego kontekstu i nie jest dostępne)
 *   - deobfuskacja Diehl LFSR dla liczników izar/PRIOS (NIE wymaga klucza AES)
 *   - generyczny parser rekordów DIF/VIF (EN 13757-3)
 *
 * Działa zarówno w przeglądarce (window.WMBus) jak i w Node (module.exports)
 * — ten sam kod jest weryfikowany w test-decoder.js na realnych wektorach.
 */
(function (root) {
  'use strict';

  // ── Pomocnicze: hex ↔ bajty ───────────────────────────────────────
  function hex2bytes(hex) {
    hex = (hex || '').replace(/[^0-9a-fA-F]/g, '');
    if (hex.length % 2 !== 0) hex = hex.slice(0, -1);
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++)
      out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  }
  function bytes2hex(b, from, to) {
    from = from || 0; to = (to == null) ? b.length : to;
    let s = '';
    for (let i = from; i < to; i++) s += b[i].toString(16).padStart(2, '0');
    return s.toUpperCase();
  }
  // uint32 big-endian (reverse=false w wmbusmeters)
  function u32be(d, o) {
    return (((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0);
  }
  // uint32 little-endian (reverse=true w wmbusmeters)
  function u32le(d, o) {
    return (((d[o]) | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0);
  }

  // ── Producent: 2 bajty → 3 litery (EN 13757) ──────────────────────
  function decodeManufacturer(m) {
    const a = ((m >> 10) & 0x1f) + 64;
    const b = ((m >> 5) & 0x1f) + 64;
    const c = (m & 0x1f) + 64;
    return String.fromCharCode(a, b, c);
  }

  // ── Typ medium / urządzenia (bajt 9) ──────────────────────────────
  const MEDIA = {
    0x00: 'inne', 0x01: 'olej', 0x02: 'elektryczność', 0x03: 'gaz',
    0x04: 'ciepło', 0x05: 'para', 0x06: 'ciepła woda', 0x07: 'woda',
    0x08: 'podzielnik (HCA)', 0x09: 'sprężone powietrze', 0x0a: 'chłód (wylot)',
    0x0b: 'chłód (wlot)', 0x0c: 'ciepło (wlot)', 0x0d: 'ciepło/chłód',
    0x0e: 'magistrala', 0x15: 'gorąca woda', 0x16: 'zimna woda',
    0x18: 'woda odpadowa', 0x28: 'ścieki', 0x37: 'gaz'
  };
  function mediumName(t) { return MEDIA[t] || ('0x' + t.toString(16)); }

  // Z medium na "typ" przyjazny używany w UI / konfiguracji
  function mediumToType(t) {
    switch (t) {
      case 0x02: return 'electricity';
      case 0x03: case 0x37: return 'gas';
      case 0x04: case 0x0c: case 0x0d: return 'heat';
      case 0x06: case 0x07: case 0x15: case 0x16: return 'water';
      default: return 'unknown';
    }
  }

  // ════════════════════════════════════════════════════════════════
  //  AES-128 (ECB block + CBC) — czysty JS
  // ════════════════════════════════════════════════════════════════
  const SBOX = (function () {
    // generacja S-box (uniknięcie 256-elementowej tablicy w źródle)
    const sbox = new Uint8Array(256);
    const inv = new Uint8Array(256);
    let p = 1, q = 1;
    do {
      p = (p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0)) & 0xff;
      q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff;
      if (q & 0x80) q ^= 0x09;
      const x = (q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^
        ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)) ^ 0x63) & 0xff;
      sbox[p] = x;
    } while (p !== 1);
    sbox[0] = 0x63;
    for (let i = 0; i < 256; i++) inv[sbox[i]] = i;
    return { sbox, inv };
  })();

  function xtime(a) { return ((a << 1) ^ ((a & 0x80) ? 0x1b : 0)) & 0xff; }
  function gmul(a, b) {
    let p = 0;
    for (let i = 0; i < 8; i++) {
      if (b & 1) p ^= a;
      const hi = a & 0x80; a = (a << 1) & 0xff; if (hi) a ^= 0x1b;
      b >>= 1;
    }
    return p & 0xff;
  }

  function keyExpansion(key) {
    const sbox = SBOX.sbox;
    const rcon = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36];
    const w = new Uint8Array(176); // 44 słowa * 4
    for (let i = 0; i < 16; i++) w[i] = key[i];
    for (let i = 16, r = 0; i < 176; i += 4) {
      let t0 = w[i - 4], t1 = w[i - 3], t2 = w[i - 2], t3 = w[i - 1];
      if (i % 16 === 0) {
        const tmp = t0; t0 = sbox[t1] ^ rcon[r++]; t1 = sbox[t2];
        t2 = sbox[t3]; t3 = sbox[tmp];
      }
      w[i] = w[i - 16] ^ t0; w[i + 1] = w[i - 15] ^ t1;
      w[i + 2] = w[i - 14] ^ t2; w[i + 3] = w[i - 13] ^ t3;
    }
    return w;
  }

  function addRoundKey(s, w, off) { for (let i = 0; i < 16; i++) s[i] ^= w[off + i]; }

  function encryptBlock(block, w) {
    const sbox = SBOX.sbox;
    const s = block.slice();
    addRoundKey(s, w, 0);
    for (let round = 1; round <= 10; round++) {
      for (let i = 0; i < 16; i++) s[i] = sbox[s[i]];
      shiftRows(s);
      if (round < 10) mixColumns(s);
      addRoundKey(s, w, round * 16);
    }
    return s;
  }
  function shiftRows(s) {
    let t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
  }
  function invShiftRows(s) {
    let t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
  }
  function mixColumns(s) {
    for (let c = 0; c < 16; c += 4) {
      const a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
      s[c] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
      s[c + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
      s[c + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
      s[c + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
    }
  }
  function invMixColumns(s) {
    for (let c = 0; c < 16; c += 4) {
      const a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
      s[c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
      s[c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
      s[c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
      s[c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
  }
  function decryptBlock(block, w) {
    const inv = SBOX.inv;
    const s = block.slice();
    addRoundKey(s, w, 160);
    for (let round = 9; round >= 0; round--) {
      invShiftRows(s);
      for (let i = 0; i < 16; i++) s[i] = inv[s[i]];
      addRoundKey(s, w, round * 16);
      if (round > 0) invMixColumns(s);
    }
    return s;
  }

  function aesCbcDecrypt(data, key, iv) {
    const w = keyExpansion(key);
    const out = new Uint8Array(data.length - (data.length % 16));
    let prev = iv.slice();
    for (let off = 0; off + 16 <= data.length; off += 16) {
      const block = data.slice(off, off + 16);
      const dec = decryptBlock(block, w);
      for (let i = 0; i < 16; i++) out[off + i] = dec[i] ^ prev[i];
      prev = block;
    }
    return out;
  }

  // Test FIPS-197 (KAT) — pewność, że implementacja AES jest poprawna
  function aesSelfTest() {
    const key = hex2bytes('000102030405060708090a0b0c0d0e0f');
    const pt = hex2bytes('00112233445566778899aabbccddeeff');
    const ct = hex2bytes('69c4e0d86a7b0430d8cdb78070b4c55a');
    const w = keyExpansion(key);
    const enc = encryptBlock(pt, w);
    const dec = decryptBlock(ct, w);
    return bytes2hex(enc) === bytes2hex(ct) && bytes2hex(dec) === bytes2hex(pt);
  }

  // ════════════════════════════════════════════════════════════════
  //  Diehl LFSR (izar / PRIOS) — port z manufacturer_specificities.cc
  // ════════════════════════════════════════════════════════════════
  const PRIOS_DEFAULT_KEYS = ['39BC8A10E66D83F8', '51728910E66D83F8'];

  function convertKey(bytes) { return (u32be(bytes, 0) ^ u32be(bytes, 4)) >>> 0; }
  function convertKeyHex(hex) { return convertKey(hex2bytes(hex)); }

  // origin == frame == surowe bajty ramki (z bajtem L na początku)
  function decodeDiehlLfsr(frame, key, checkByte) {
    key = (key ^ u32be(frame, 2)) >>> 0;   // producent + adres[0-1]
    key = (key ^ u32be(frame, 6)) >>> 0;   // adres[2-3] + wersja + typ
    key = (key ^ u32be(frame, 10)) >>> 0;  // CI + kolejne bajty nagłówka
    const size = frame.length - 15;
    if (size <= 0) return null;
    const decoded = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
      for (let j = 0; j < 8; j++) {
        const bit = (((key >>> 1) & 1) ^ ((key >>> 2) & 1) ^
          ((key >>> 11) & 1) ^ ((key >>> 31) & 1)) & 1;
        key = (((key << 1) >>> 0) | bit) >>> 0;
      }
      decoded[i] = frame[15 + i] ^ (key & 0xff);
      if (i === 0 && decoded[0] !== checkByte) return null;
    }
    return decoded;
  }

  // Dekoder licznika izar (woda Diehl/PRIOS) — bez klucza AES
  function decodeIzar(frame, userKeyHex) {
    const keys = [];
    if (userKeyHex && userKeyHex.replace(/[^0-9a-fA-F]/g, '').length === 16)
      keys.push(convertKeyHex(userKeyHex));
    for (const k of PRIOS_DEFAULT_KEYS) keys.push(convertKeyHex(k));

    let decoded = null;
    for (const key of keys) {
      decoded = decodeDiehlLfsr(frame, key, 0x4b); // HEADER_1_BYTE, 0x4B
      if (decoded) break;
    }
    if (!decoded) return null;

    const fields = [];
    const total_l = u32le(decoded, 1);
    fields.push({ name: 'total', value: total_l / 1000, unit: 'm³' });
    if (decoded.length > 8) {
      const last_l = u32le(decoded, 5);
      fields.push({ name: 'last_month_total', value: last_l / 1000, unit: 'm³' });
    }
    if (decoded.length > 10) {
      let yy = ((decoded[10] & 0xf0) >> 1) + ((decoded[9] & 0xe0) >> 5);
      yy = yy > 80 ? yy + 1900 : yy + 2000;
      const mm = decoded[10] & 0x0f;
      const dd = decoded[9] & 0x1f;
      fields.push({
        name: 'last_month_measure_date',
        value: yy + '-' + String(mm % 99).padStart(2, '0') + '-' + String(dd % 99).padStart(2, '0'),
        unit: ''
      });
    }
    return { driver: 'izar', fields: fields, decryptedHex: bytes2hex(decoded) };
  }

  // ════════════════════════════════════════════════════════════════
  //  Generyczny parser rekordów DIF/VIF (EN 13757-3)
  // ════════════════════════════════════════════════════════════════
  const TIME_UNIT = ['s', 'min', 'h', 'd'];

  function primaryVif(vif) {
    const v = vif & 0x7f;
    if (v <= 0x07) return { q: 'energy', unit: 'Wh', exp: v - 3 };
    if (v <= 0x0f) return { q: 'energy', unit: 'J', exp: v - 0x08 };
    if (v <= 0x17) return { q: 'volume', unit: 'm³', exp: (v - 0x10) - 6 };
    if (v <= 0x1f) return { q: 'mass', unit: 'kg', exp: (v - 0x18) - 3 };
    if (v <= 0x23) return { q: 'on_time', time: TIME_UNIT[v - 0x20] };
    if (v <= 0x27) return { q: 'operating_time', time: TIME_UNIT[v - 0x24] };
    if (v <= 0x2f) return { q: 'power', unit: 'W', exp: (v - 0x28) - 3 };
    if (v <= 0x37) return { q: 'power', unit: 'J/h', exp: (v - 0x30) };
    if (v <= 0x3f) return { q: 'volume_flow', unit: 'm³/h', exp: (v - 0x38) - 6 };
    if (v <= 0x47) return { q: 'volume_flow', unit: 'm³/min', exp: (v - 0x40) - 7 };
    if (v <= 0x4f) return { q: 'volume_flow', unit: 'm³/s', exp: (v - 0x48) - 9 };
    if (v <= 0x57) return { q: 'mass_flow', unit: 'kg/h', exp: (v - 0x50) - 3 };
    if (v <= 0x5b) return { q: 'flow_temperature', unit: '°C', exp: (v - 0x58) - 3 };
    if (v <= 0x5f) return { q: 'return_temperature', unit: '°C', exp: (v - 0x5c) - 3 };
    if (v <= 0x63) return { q: 'temperature_difference', unit: 'K', exp: (v - 0x60) - 3 };
    if (v <= 0x67) return { q: 'external_temperature', unit: '°C', exp: (v - 0x64) - 3 };
    if (v <= 0x6b) return { q: 'pressure', unit: 'bar', exp: (v - 0x68) - 3 };
    if (v === 0x6c) return { q: 'date', dateType: 'G' };
    if (v === 0x6d) return { q: 'datetime', dateType: 'F' };
    if (v <= 0x73) return { q: 'averaging_duration', time: TIME_UNIT[v - 0x70] };
    if (v <= 0x77) return { q: 'actuality_duration', time: TIME_UNIT[v - 0x74] };
    if (v === 0x78) return { q: 'fabrication_no' };
    if (v === 0x79) return { q: 'enhanced_id' };
    if (v === 0x7a) return { q: 'bus_address' };
    return null;
  }
  // Tablica rozszerzona 0xFD (m.in. napięcie/prąd dla liczników energii)
  function fdVif(vife) {
    const v = vife & 0x7f;
    if (v >= 0x40 && v <= 0x47) return { q: 'voltage', unit: 'V', exp: (v - 0x40) - 9 };
    if (v >= 0x48 && v <= 0x4f) return { q: 'current', unit: 'A', exp: (v - 0x48) - 12 };
    if (v === 0x0c) return { q: 'model_version' };
    if (v === 0x0b) return { q: 'parameter_set' };
    if (v === 0x17 || v === 0x18) return { q: 'error_flags' };
    return null;
  }

  const DATA_LEN = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 4, 6: 6, 7: 8, 8: 0, 9: 1, 10: 2, 11: 3, 12: 4, 13: -1, 14: 6, 15: 0 };

  function readSignedLE(d, o, n) {
    let v = 0;
    for (let i = 0; i < n; i++) v += d[o + i] * Math.pow(2, 8 * i);
    const max = Math.pow(2, 8 * n);
    if (v >= max / 2) v -= max;   // dopełnienie do dwójki
    return v;
  }
  function readBCD(d, o, n) {
    let s = '';
    for (let i = n - 1; i >= 0; i--) s += d[o + i].toString(16).padStart(2, '0');
    return parseInt(s, 10);
  }
  function readFloat32(d, o) {
    const buf = new ArrayBuffer(4), dv = new DataView(buf);
    for (let i = 0; i < 4; i++) dv.setUint8(i, d[o + i]);
    return dv.getFloat32(0, true);
  }
  function decodeDateG(d, o) {
    const day = d[o] & 0x1f;
    const mon = d[o + 1] & 0x0f;
    let yr = ((d[o] & 0xe0) >> 5) | ((d[o + 1] & 0xf0) >> 1);
    yr += (yr < 81) ? 2000 : 1900;
    return yr + '-' + String(mon).padStart(2, '0') + '-' + String(day).padStart(2, '0');
  }

  // Zwraca listę rekordów {name,value,unit} z payloadu (po deszyfracji)
  function parseDifVif(d) {
    const out = [];
    let i = 0;
    let guard = 0;
    while (i < d.length && guard++ < 64) {
      let dif = d[i++];
      if (dif === 0x2f) continue;                 // wypełniacz
      if (dif === 0x0f || dif === 0x1f) break;     // dane producenta — koniec parsowania
      const dataField = dif & 0x0f;
      const func = (dif >> 4) & 0x03;              // 0=akt,1=max,2=min,3=błąd
      let storage = (dif >> 6) & 0x01;
      let tariff = 0, deviceUnit = 0, sn = 0;
      // DIFE
      while (dif & 0x80) {
        if (i >= d.length) return out;
        dif = d[i++];
        storage |= (dif & 0x0f) << (1 + sn * 4);
        tariff |= ((dif >> 4) & 0x03) << (sn * 2);
        deviceUnit |= ((dif >> 6) & 0x01) << sn;
        sn++;
      }
      if (i >= d.length) break;
      let vif = d[i++];
      let info = null, plainUnit = null;
      if (vif === 0x7c || vif === 0xfc) {          // VIF tekstowy (ASCII)
        if (i >= d.length) break;
        const ulen = d[i++]; let u = '';
        if (i + ulen > d.length) break;
        for (let k = ulen - 1; k >= 0; k--) u += String.fromCharCode(d[i + k]);
        i += ulen; plainUnit = u; info = { q: 'value', unit: u };
      } else if ((vif & 0x7f) === 0x7d || (vif & 0x7f) === 0x7b) {
        // odczyt rozszerzony — pomiń jeden VIFE i potraktuj generycznie
        const vife = d[i++]; info = fdVif(vife) || { q: 'value' };
      } else if ((vif & 0x7f) === 0x7f) {
        info = { q: 'manufacturer_specific' };
      } else {
        info = primaryVif(vif);
        // VIFE
        let firstVife = -1;
        while (vif & 0x80) {
          if (i >= d.length) break;
          vif = d[i++]; if (firstVife < 0) firstVife = vif;
        }
        if (!info && firstVife >= 0) info = fdVif(firstVife);
      }
      if (!info) info = { q: 'unknown_vif_0x' + (d[i - 1]).toString(16) };

      // Odczyt wartości (z kontrolą zakresu — ramki bywają uszkodzone)
      let len = DATA_LEN[dataField];
      let value, raw;
      if (len === -1) { // LVAR
        if (i >= d.length) break;
        const lv = d[i++];
        if (lv < 0xc0) { // ASCII
          if (i + lv > d.length) break;
          let s = ''; for (let k = lv - 1; k >= 0; k--) s += String.fromCharCode(d[i + k]);
          i += lv; value = s; raw = s;
        } else { len = lv & 0x0f; if (i + len > d.length) break; value = readBCD(d, i, len); i += len; }
      } else if (len > 0 && i + len > d.length) {
        break;                                   // brakuje bajtów na wartość
      } else if (dataField === 0x05) {           // float32
        value = readFloat32(d, i); raw = value; i += 4;
      } else if (dataField >= 0x09 && dataField <= 0x0e) { // BCD
        raw = readBCD(d, i, len); value = raw; i += len;
      } else if (len > 0) {
        raw = readSignedLE(d, i, len); value = raw; i += len;
      } else { value = null; }

      if (info.dateType === 'G' && len >= 2) value = decodeDateG(d, i - len);
      else if (typeof value === 'number' && info.exp != null)
        value = value * Math.pow(10, info.exp);

      // Nazwa pola: jakość + storage/tariff/funkcja
      let name = info.q || 'value';
      if (func === 1) name += '_max';
      else if (func === 2) name += '_min';
      if (storage === 1) name = 'target_' + name;
      else if (storage > 1) name += '_s' + storage;
      if (tariff > 0) name += '_t' + tariff;
      if (info.time) info.unit = info.time;

      out.push({ name: name, value: value, unit: info.unit || '' });
    }
    return out;
  }

  // ════════════════════════════════════════════════════════════════
  //  Wysokopoziomowy dekoder ramki
  // ════════════════════════════════════════════════════════════════
  function buildMode5IV(frame, addr) {
    // IV = producent(2) + adres(4) + wersja + typ + ACC×8
    const iv = new Uint8Array(16);
    for (let i = 0; i < 8; i++) iv[i] = frame[addr.ivStart + i];
    for (let i = 8; i < 16; i++) iv[i] = addr.acc;
    return iv;
  }

  function decodeFrame(hex, opts) {
    opts = opts || {};
    const frame = hex2bytes(hex);
    const res = { ok: false, raw: bytes2hex(frame), fields: [], warnings: [] };
    if (frame.length < 11) { res.error = 'Ramka za krótka'; return res; }

    const L = frame[0], C = frame[1];
    const M = frame[2] | (frame[3] << 8);
    const manufacturer = decodeManufacturer(M);
    const id = bytes2hex([frame[7], frame[6], frame[5], frame[4]]);
    const version = frame[8];
    const type = frame[9];
    const ci = frame[10];

    res.link = {
      L: L, C: C, manufacturer: manufacturer, id: id,
      version: version, type: type, medium: mediumName(type),
      typeName: mediumToType(type), ci: ci
    };

    // długość kontrolna (L liczy bajty po polu L)
    if (frame.length !== L + 1)
      res.warnings.push('Długość ramki (' + frame.length + 'B) ≠ L+1 (' + (L + 1) + 'B)');

    // ── 1) Diehl PRIOS / izar (CI 0xA0–0xA7) — brak klucza AES ──────
    if (ci >= 0xa0 && ci <= 0xa7) {
      const izar = decodeIzar(frame, opts.key);
      if (izar) {
        res.ok = true;
        res.driver = izar.driver;
        res.security = { scheme: 'prios', needKey: false, label: 'Diehl PRIOS (LFSR)' };
        res.fields = izar.fields;
        res.decryptedHex = izar.decryptedHex;
        return res;
      }
      res.security = { scheme: 'prios', needKey: false, label: 'Diehl PRIOS (LFSR)' };
      res.error = 'Nie udało się zdeobfuskować PRIOS (zły klucz lub nieobsługiwany wariant)';
      return res;
    }

    // ── 2) Standardowy nagłówek TPL (CI 0x7A krótki / 0x72 długi) ───
    let payloadStart = -1, acc = 0, mode = 0, ivStart = 2;
    if (ci === 0x7a) {            // krótki TPL
      acc = frame[11]; const cfg = frame[13] | (frame[14] << 8);
      mode = (cfg >> 8) & 0x1f; payloadStart = 15; ivStart = 2; // adres z warstwy łącza
    } else if (ci === 0x72) {    // długi TPL
      acc = frame[19]; const cfg = frame[21] | (frame[22] << 8);
      mode = (cfg >> 8) & 0x1f; payloadStart = 23; ivStart = 11; // adres z TPL
    } else if (ci === 0x78 || ci === 0x79) { // brak nagłówka / MBus
      payloadStart = 11; mode = 0;
    } else {
      res.security = { scheme: 'unsupported', needKey: false, label: 'CI 0x' + ci.toString(16) };
      res.error = 'Nieobsługiwany nagłówek CI=0x' + ci.toString(16);
      return res;
    }

    if (mode === 0) {            // dane otwarte
      res.ok = true;
      res.driver = 'generic';
      res.security = { scheme: 'plain', needKey: false, label: 'Otwarta (bez szyfrowania)' };
      let payload = frame.slice(payloadStart);
      // pomiń wiodące 2F (wypełniacz) zostawiając parserowi
      res.fields = parseDifVif(payload);
      res.decryptedHex = bytes2hex(payload);
      return res;
    }

    if (mode === 5) {            // AES-128 CBC, tryb 5
      res.security = { scheme: 'aes5', needKey: true, label: 'AES-128 tryb 5' };
      const keyHex = (opts.key || '').replace(/[^0-9a-fA-F]/g, '');
      if (keyHex.length !== 32) {
        res.needKey = true;
        res.error = 'Wymagany klucz AES-128 (32 znaki hex)';
        return res;
      }
      const key = hex2bytes(keyHex);
      const iv = buildMode5IV(frame, { ivStart: ivStart, acc: acc });
      const enc = frame.slice(payloadStart);
      if (enc.length < 16) { res.error = 'Za krótki payload do deszyfracji'; return res; }
      const dec = aesCbcDecrypt(enc, key, iv);
      // Poprawna deszyfracja OMS zaczyna się od 2F2F
      if (!(dec[0] === 0x2f && dec[1] === 0x2f))
        res.warnings.push('Brak znacznika 2F2F po deszyfracji — możliwy zły klucz');
      res.ok = (dec[0] === 0x2f && dec[1] === 0x2f);
      res.driver = 'generic';
      res.fields = parseDifVif(dec);
      res.decryptedHex = bytes2hex(dec);
      if (!res.ok && res.fields.length === 0)
        res.error = 'Deszyfracja nieudana — sprawdź klucz';
      return res;
    }

    res.security = { scheme: 'mode' + mode, needKey: true, label: 'Tryb zabezpieczeń ' + mode };
    res.error = 'Tryb zabezpieczeń ' + mode + ' nie jest obsługiwany';
    return res;
  }

  const WMBus = {
    hex2bytes, bytes2hex, decodeManufacturer, mediumName, mediumToType,
    keyExpansion, encryptBlock, decryptBlock, aesCbcDecrypt, aesSelfTest,
    convertKeyHex, decodeDiehlLfsr, decodeIzar, parseDifVif, decodeFrame,
    PRIOS_DEFAULT_KEYS
  };

  root.WMBus = WMBus;
  if (typeof module !== 'undefined' && module.exports) module.exports = WMBus;
})(typeof window !== 'undefined' ? window : this);
