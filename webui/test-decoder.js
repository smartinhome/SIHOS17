/*
 * test-decoder.js — weryfikacja wmbus-decoder.js na realnych wektorach
 * z projektu wmbusmeters (ten sam kod, który ładuje przeglądarka).
 *
 *   node webui/test-decoder.js
 */
const WMBus = require('./wmbus-decoder.js');

let pass = 0, fail = 0;
function ok(cond, label, extra) {
  if (cond) { pass++; console.log('  ✓ ' + label); }
  else { fail++; console.log('  ✗ ' + label + (extra ? '  → ' + extra : '')); }
}
function near(a, b, eps) { return Math.abs(a - b) <= (eps || 0.0005); }
function field(res, name) { const f = res.fields.find(x => x.name === name); return f ? f.value : undefined; }
function byUnit(res, unit) { const f = res.fields.find(x => x.unit === unit); return f ? f.value : undefined; }

console.log('\n== AES-128 (FIPS-197 KAT) ==');
ok(WMBus.aesSelfTest(), 'szyfrowanie + deszyfracja bloku zgodne z FIPS-197');

console.log('\n== Producent / link layer ==');
(function () {
  const r = WMBus.decodeFrame('1944304C72242421D401A2013D4013DD8B46A4999C1293E582CC');
  ok(r.link.manufacturer === 'SAP', 'manufacturer SAP', r.link.manufacturer);
  ok(r.link.id === '21242472', 'id 21242472', r.link.id);
  ok(WMBus.decodeManufacturer(0x4cae) === 'SEN', 'manufacturer SEN (0x4CAE)');
})();

console.log('\n== izar (Diehl LFSR, bez klucza) ==');
const izarVectors = [
  ['1944304C72242421D401A2013D4013DD8B46A4999C1293E582CC', 3.488, 3.486],
  ['2944A511780729662366A20118001378D3B3DB8CEDD77731F25832AAF3DA8CADF9774EA673172E8C61F2', 16.76, 11.84],
  ['1944A511780779194820A121170013355F8EDB2D03C6912B1E37', 4.366, 0],
  ['1944304c9c5824210c04a363140013716577ec59e8663ab0d31c', 38.944, 38.691],
  ['1944304CDEFFE420CC01A263120013258F907B0AFF12529AC33B', 159.832, 157.76],
  ['19442423860775035048A251520015BEB6B2E1ED623A18FC74A5', 521.602, 519.147],
];
izarVectors.forEach(([hex, total, last], idx) => {
  const r = WMBus.decodeFrame(hex);
  const t = field(r, 'total');
  const lm = field(r, 'last_month_total');
  ok(r.ok && r.driver === 'izar' && near(t, total, 0.001),
    'IzarWater' + (idx + 1) + ' total=' + total + ' m³', 'got ' + t);
  ok(near(lm, last, 0.001),
    'IzarWater' + (idx + 1) + ' last_month=' + last + ' m³', 'got ' + lm);
});

console.log('\n== Generyczny DIF/VIF (iperl, dane otwarte) ==');
(function () {
  const r = WMBus.decodeFrame('1E44AE4C9956341268077A360010002F2F0413181E0000023B00002F2F2F2F');
  ok(r.link.manufacturer === 'SEN', 'manufacturer SEN', r.link.manufacturer);
  ok(r.ok && r.security.scheme === 'plain', 'rozpoznane jako otwarte (tryb 0)');
  const vol = byUnit(r, 'm³');
  ok(near(vol, 7.704), 'total volume = 7.704 m³', 'got ' + vol);
})();

console.log('\n== Generyczny DIF/VIF — rekordy (energia/moc/temp/data/BCD) ==');
(function () {
  const rec = (hex) => WMBus.parseDifVif(WMBus.hex2bytes(hex));
  // DIF 04 (u32) VIF 03 (energia, Wh, exp0): 0x2710 = 10000 Wh
  let r = rec('0403102700 00'.replace(/\s/g, ''));
  ok(r[0] && r[0].name === 'energy' && near(r[0].value, 10000, 0.5) && r[0].unit === 'Wh',
    'energia 10000 Wh', JSON.stringify(r[0]));
  // DIF 02 (u16) VIF 2B (moc W exp0): 100 W
  r = rec('022B6400');
  ok(r[0] && r[0].name === 'power' && near(r[0].value, 100, 0.5) && r[0].unit === 'W',
    'moc 100 W', JSON.stringify(r[0]));
  // DIF 02 VIF 5A (temp. zasilania °C exp-1): 0x00C8=200 -> 20.0 °C
  r = rec('025AC800');
  ok(r[0] && r[0].name === 'flow_temperature' && near(r[0].value, 20.0) && r[0].unit === '°C',
    'temperatura zasilania 20.0 °C', JSON.stringify(r[0]));
  // DIF 02 VIF 6C (data typ G): 0x9F 0x2C -> 2020-12-31
  r = rec('026C9F2C');
  ok(r[0] && r[0].name === 'date' && r[0].value === '2020-12-31',
    'data typ G 2020-12-31', JSON.stringify(r[0]));
  // DIF 0C (BCD 8 cyfr) VIF 13 (objętość, exp-3): BCD LE 45 23 01 00 -> 12345 -> 12.345 m³
  r = rec('0C1345230100');
  ok(r[0] && r[0].name === 'volume' && near(r[0].value, 12.345) && r[0].unit === 'm³',
    'objętość BCD 12.345 m³', JSON.stringify(r[0]));
})();

console.log('\n== Wykrywanie "wymagany klucz" ==');
(function () {
  // sztuczna ramka tryb 5 (cfg = 0x0500 → mode 5) bez klucza → needKey
  const r = WMBus.decodeFrame('2544AE4C9956341268077A3600' + '0005' + '00112233445566778899AABBCCDDEEFF00112233445566');
  ok(r.security && r.security.scheme === 'aes5' && r.needKey === true,
    'ramka tryb 5 bez klucza zgłasza needKey', JSON.stringify(r.security));
})();

console.log('\n----------------------------------------');
console.log('  WYNIK: ' + pass + ' pass, ' + fail + ' fail');
console.log('----------------------------------------\n');
process.exit(fail ? 1 : 0);
