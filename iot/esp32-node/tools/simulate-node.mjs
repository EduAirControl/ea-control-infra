#!/usr/bin/env node
/*
 * Simulador del nodo ESP32 - doble de prueba del firmware.
 * ============================================================
 *
 * Ejecuta la parte HTTP del firmware (src/main.cpp) contra el stack real, para
 * poder probar el backend sin la placa fisica. Reproduce:
 *
 *   APP  -> ms-security    POST /api/v1/auth/login                        (JWT ADMIN)
 *   APP  -> sensor-mgmt    GET  /api/v1/sensors
 *   APP  -> sensor-mgmt    GET  /api/v1/sensor-installations?active=true
 *   APP  -> sensor-mgmt    POST /api/v1/devices
 *   APP  -> ms-security    POST /api/v1/auth/device/provision-tokens
 *   APP  -> (BLE)          entrega el token            <-- aqui no hay BLE
 *   NODO -> ms-security    POST /api/v1/auth/device/provision           (sin JWT)
 *   NODO -> ms-security    POST /oauth2/token           (client_credentials)
 *   NODO -> gateway        POST /api/v1/measurements    xN, una por variable
 *
 * NO reproduce, y solo se prueba con la placa:
 *   - el transporte BLE. Ningun navegador puede hacer de periferico (Web
 *     Bluetooth es solo rol central), asi que un simulador web de ESP32 no existe.
 *   - la lectura de GPIO5 (DHT11) y GPIO34 (KY-038).
 *
 * Uso:
 *   ENVIRONMENT_ID=<uuid> node tools/simulate-node.mjs
 *   ENVIRONMENT_ID=<uuid> node tools/simulate-node.mjs --loop
 *   node tools/simulate-node.mjs --only-publish <token>
 */

const CANONICAL_VARIABLE_IDS = {
  temperature: '00000000-0000-4000-8000-000000000071',
  humidity: '00000000-0000-4000-8000-000000000072',
  co2: '00000000-0000-4000-8000-000000000073',
  noise: '00000000-0000-4000-8000-000000000074',
};

/**
 * Sufijo del serial -> variable. El seed crea los seriales como
 * 'EA-' || ambiente || '-' || inicial(variable) (101-seed-dev-sensors.sql), y la
 * inicial es la unica pista que liga una instalacion con su variable: el endpoint
 * de instalaciones no devuelve el serial.
 */
const SERIAL_SUFFIX_TO_CODE = { T: 'temperature', H: 'humidity', C: 'co2', N: 'noise' };

const CFG = {
  gatewayBaseUrl: env('GATEWAY_URL', 'http://localhost:8080'),
  securityBaseUrl: env('SECURITY_URL', 'http://localhost:8081'),
  email: env('ADMIN_EMAIL', 'admin@eduaircontrol.com'),
  password: env('ADMIN_PASSWORD', 'EduAirControl1!'),
  environmentId: env('ENVIRONMENT_ID', ''),
  mac: env('DEVICE_MAC', ''),
  sampleIntervalMs: Number(env('SAMPLE_INTERVAL_MS', '30000')),
};

function env(key, fallback) {
  return process.env[key] ?? fallback;
}

function round1(n) {
  return Math.round(n * 10) / 10;
}

function log(stage, message) {
  const ts = new Date().toISOString().slice(11, 19);
  console.log(`[${ts}] ${stage.padEnd(5)} ${message}`);
}

function fail(message, detail) {
  console.error(`\n  FALLO: ${message}`);
  if (detail) {
    console.error(detail.replace(/^/gm, '  ') + '\n');
  }
  process.exit(1);
}

// ---------------------------------------------------------------------------
// Cliente HTTP: replica SecureHttp del firmware, incluidos los codigos de estado
// que alli tienen significado propio.
// ---------------------------------------------------------------------------
async function http(method, url, { headers = {}, body, basic } = {}) {
  const finalHeaders = { ...headers };
  if (body !== undefined && !finalHeaders['Content-Type']) {
    finalHeaders['Content-Type'] = 'application/json';
  }
  if (basic) {
    finalHeaders.Authorization = 'Basic ' + Buffer.from(basic).toString('base64');
  }

  let response;
  try {
    response = await fetch(url, { method, headers: finalHeaders, body, signal: AbortSignal.timeout(15000) });
  } catch (error) {
    return { status: -1, text: `sin conexion: ${error.message}`, json: null, ok: false };
  }
  const text = await response.text();
  let json = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    json = null;
  }
  return { status: response.status, text, json, ok: response.ok };
}

// ===========================================================================
// ETAPA APP - lo que haria el celular con un ADMIN
// ===========================================================================
async function login() {
  log('APP', `login como ${CFG.email}`);
  const res = await http('POST', `${CFG.securityBaseUrl}/api/v1/auth/login`, {
    body: JSON.stringify({ email: CFG.email, password: CFG.password }),
  });
  // ms-security devuelve accessToken. Se acepta tambien `token` porque el
  // monolito usaba esa clave y alguna app movil todavia la espera.
  const token = res.json?.accessToken ?? res.json?.token;
  if (!res.ok || !token) {
    fail('login fallido', `HTTP ${res.status}: ${res.text}`);
  }
  log('APP', `  JWT obtenido (${token.length} chars)`);
  return token;
}

/**
 * Resuelve los bindings (variable canonica + instalacion) leyendo el ambiente.
 *
 * Dos llamadas porque sensor-installation no expone el serial: primero se cargan
 * los sensores para tener sensorId -> serialNumber, luego las instalaciones ya
 * filtradas por ambiente. De ahi sale el par.
 */
async function resolveBindings(jwt, environmentId) {
  log('APP', `leyendo sensores e instalaciones del ambiente ${environmentId}`);
  const auth = { headers: { Authorization: `Bearer ${jwt}` } };

  const sensorsRes = await http('GET', `${CFG.gatewayBaseUrl}/api/v1/sensors?page=1&limit=100`, auth);
  if (!sensorsRes.ok) {
    fail('no se pudieron listar los sensores', `HTTP ${sensorsRes.status}: ${sensorsRes.text}`);
  }
  const serialBySensorId = new Map();
  for (const sensor of sensorsRes.json?.data ?? []) {
    serialBySensorId.set(sensor.sensorId, sensor.serialNumber);
  }
  log('APP', `  ${serialBySensorId.size} sensor(es) en el catalogo`);

  const instRes = await http(
    'GET',
    `${CFG.gatewayBaseUrl}/api/v1/sensor-installations` +
      `?educationalEnvironmentId=${environmentId}&active=true&page=1&limit=100`,
    auth,
  );
  if (!instRes.ok) {
    fail('no se pudieron listar las instalaciones', `HTTP ${instRes.status}: ${instRes.text}`);
  }
  const installations = instRes.json?.data ?? [];
  if (installations.length === 0) {
    fail(
      'el ambiente no tiene sensores instalados',
      'Hace falta sensor + sensor-variable + sensor-installation.\n' +
        'Ver README, seccion Prerrequisitos, paso 2.',
    );
  }

  const bindings = [];
  const sinSerial = [];
  const yaVistos = new Set();
  for (const installation of installations) {
    const serial = serialBySensorId.get(installation.sensorId);
    const suffix = serial ? serial.trim().toUpperCase().split('-').pop() : '';
    const code = SERIAL_SUFFIX_TO_CODE[suffix];
    if (!code) {
      sinSerial.push(serial ?? `sensorId=${installation.sensorId}`);
      continue;
    }
    // Una sola instalacion por variable: environment_measurement no distingue
    // mas y dos sensores de la misma variable solo duplicarian filas.
    if (yaVistos.has(code)) {
      log('APP', `  aviso: segunda instalacion de ${code}; se ignora`);
      continue;
    }
    yaVistos.add(code);
    bindings.push({
      variableId: CANONICAL_VARIABLE_IDS[code],
      sensorInstallationId: installation.sensorInstallationId,
      code,
    });
  }

  if (sinSerial.length) {
    console.warn(
      `\n  aviso: ${sinSerial.length} instalacion(es) sin serial reconocible (${sinSerial.join(', ')}).` +
        '\n  Se espera el formato EA-<ambiente>-<T|H|C|N>. Edita SERIAL_SUFFIX_TO_CODE si tu convencion es otra.\n',
    );
  }
  log('APP', `  ${bindings.length} binding(s): ${bindings.map((b) => b.code).join(', ') || 'ninguno'}`);
  return bindings;
}

async function registerDevice(jwt, mac, environmentId) {
  log('APP', `registrando dispositivo ${mac}`);
  const res = await http('POST', `${CFG.gatewayBaseUrl}/api/v1/devices`, {
    headers: { Authorization: `Bearer ${jwt}` },
    body: JSON.stringify({
      macAddress: mac,
      name: 'Nodo simulado',
      deviceType: 'esp32',
      educationalEnvironmentId: environmentId,
    }),
  });
  if (res.status === 409) {
    fail(
      'ya existe un dispositivo con esa MAC',
      'Cambia DEVICE_MAC o borra el registro previo con DELETE /api/v1/devices/{id}.',
    );
  }
  if (!res.ok) {
    fail('alta de dispositivo fallida', `HTTP ${res.status}: ${res.text}`);
  }
  log('APP', `  deviceId=${res.json.deviceId}`);
  return res.json.deviceId;
}

async function requestProvisionToken(jwt, deviceId, mac, bindings) {
  log('APP', `pidiendo token de aprovisionamiento (${bindings.length} binding(s))`);
  const res = await http('POST', `${CFG.securityBaseUrl}/api/v1/auth/device/provision-tokens`, {
    headers: { Authorization: `Bearer ${jwt}` },
    body: JSON.stringify({
      deviceId,
      macAddress: mac,
      bindings: bindings.map((b) => ({
        variableId: b.variableId,
        sensorInstallationId: b.sensorInstallationId,
      })),
    }),
  });
  if (!res.ok) {
    fail('emision del token fallida', `HTTP ${res.status}: ${res.text}`);
  }
  log('APP', `  client_id=${res.json.clientId}`);
  log('APP', `  vence      ${res.json.expiresAt}`);
  return res.json;
}

// ===========================================================================
// ETAPA NODO - lo que hace el ESP32 una vez que tiene el token
// ===========================================================================

/**
 * Canje del token. El firmware guardaria el resultado en NVS; aqui se queda en
 * memoria, y el codigo de variable se reasocia por variableId porque el token
 * transporta UUID y no nombres.
 */
async function redeemProvisionToken(token) {
  log('NODO', 'canjeando token de aprovisionamiento');
  const res = await http('POST', `${CFG.securityBaseUrl}/api/v1/auth/device/provision`, {
    body: JSON.stringify({ token }),
  });
  if (!res.ok) {
    fail(
      'canje fallido',
      `HTTP ${res.status}: ${res.text}\n` +
        '  409 = el token ya se uso (es de un solo uso, pide uno nuevo)\n' +
        '  400 = fallo la firma o vencio',
    );
  }
  const config = res.json;
  config.bindings = config.bindings.map((b) => ({
    ...b,
    code: inferCode(b.variableId),
  }));
  log('NODO', `  security  ${config.securityBaseUrl}`);
  log('NODO', `  gateway   ${config.gatewayBaseUrl}`);
  log('NODO', `  tokenUrl  ${config.tokenUrl}`);
  log('NODO', `  mac      ${config.macAddress}`);
  log('NODO', `  bindings ${config.bindings.map((b) => b.code).join(', ')}`);
  return config;
}

async function fetchAccessToken(config) {
  log('NODO', 'pidiendo access token (client_credentials)');
  const res = await http('POST', config.tokenUrl, {
    headers: { 'Content-Type': 'application/x-www-form-urlencoded', Accept: 'application/json' },
    body: 'grant_type=client_credentials&scope=sensors.write',
    basic: `${config.clientId}:${config.clientSecret}`,
  });
  if (!res.ok) {
    fail(
      'no se pudo obtener access token',
      `HTTP ${res.status}: ${res.text}\n` +
        '  401 = ms-security no conoce ese client_id, o la credencial ya fue rotada.',
    );
  }
  log('NODO', `  expira en ${res.json.expires_in} s`);
  return res.json.access_token;
}

const UNITS = { temperature: 'C', humidity: '%', noise: 'dB', co2: 'ppm' };

function sampleFor(code) {
  switch (code) {
    case 'temperature':
      return round1(20 + Math.random() * 6);
    case 'humidity':
      return round1(45 + Math.random() * 25);
    case 'noise':
      return round1(35 + Math.random() * 40);
    case 'co2':
      return Math.round(400 + Math.random() * 600);
    default:
      return 0;
  }
}

/** Una lectura por POST, como exige MeasurementController. */
async function publishSample(config, accessToken) {
  const measuredAt = new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
  let accepted = 0;

  for (const binding of config.bindings) {
    const value = sampleFor(binding.code);
    const res = await http('POST', `${config.gatewayBaseUrl}${config.measurementsPath}`, {
      headers: { Authorization: `Bearer ${accessToken}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({
        sensorInstallationId: binding.sensorInstallationId,
        variableId: binding.variableId,
        value,
        measuredAt,
      }),
    });

    if (res.ok) {
      accepted++;
      // 201 con id null es una reingesta: no es un fallo (MeasurementService.record
      // devuelve null si ya existe la tripla instalacion/variable/instante).
      const id = res.json?.id ?? null;
      log('NODO', `  ${binding.code.padEnd(11)} ${String(value).padStart(6)} ${(UNITS[binding.code] ?? '').padEnd(3)} -> 201${id ? '' : ' (reingesta)'}`);
    } else {
      log('NODO', `  ${binding.code.padEnd(11)} ${String(value).padStart(6)} ${(UNITS[binding.code] ?? '').padEnd(3)} -> HTTP ${res.status} ${res.text.slice(0, 150)}`);
      if (res.status === 401) log('NODO', '  el gateway rechazo el token; el firmware lo renovaria');
      if (res.status === 409) log('NODO', '  409 = falta sensor instalado y activo en el ambiente');
      if (res.status === 429) log('NODO', '  429 = rate limit del gateway');
    }
  }
  return accepted;
}

function inferCode(variableId) {
  return Object.entries(CANONICAL_VARIABLE_IDS).find(([, id]) => id === variableId)?.[0] ?? 'desconocido';
}

function randomMac() {
  return Array.from({ length: 6 }, () =>
    Math.floor(Math.random() * 256).toString(16).padStart(2, '0').toUpperCase(),
  ).join(':');
}

function argValue(args, flag) {
  const i = args.indexOf(flag);
  return i >= 0 ? args[i + 1] : null;
}

function usage() {
  console.log(`
Simulador del nodo ESP32 (doble de prueba del firmware, sin placa fisica)

  ENVIRONMENT_ID=<uuid> node tools/simulate-node.mjs [--loop]
  node tools/simulate-node.mjs --only-publish <token>

Opciones
  --loop              publica cada SAMPLE_INTERVAL_MS hasta Ctrl-C
  --only-publish TOK  salta la etapa APP: solo canjea el token y publica
  --mac AA:BB:...     MAC del nodo simulado
  --help

Entorno
  GATEWAY_URL         por defecto http://localhost:8080
  SECURITY_URL        por defecto http://localhost:8081
  ADMIN_EMAIL         por defecto admin@eduaircontrol.com
  ADMIN_PASSWORD      por defecto EduAirControl1!
  ENVIRONMENT_ID      obligatorio: ambiente con sensores instalados y activos
  DEVICE_MAC          por defecto aleatorio
  SAMPLE_INTERVAL_MS  por defecto 30000
`);
}

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    usage();
    return;
  }

  let config;

  const publishIndex = args.indexOf('--only-publish');
  if (publishIndex >= 0) {
    const token = args[publishIndex + 1];
    if (!token) {
      usage();
      process.exit(1);
    }
    config = await redeemProvisionToken(token);
  } else {
    if (!CFG.environmentId) {
      console.error('\n  Falta ENVIRONMENT_ID.\n');
      usage();
      process.exit(1);
    }
    const mac = argValue(args, '--mac') ?? CFG.mac ?? randomMac();

    console.log('\n=== EduAirControl: simulacion del nodo ESP32 ===\n');
    const jwt = await login();
    const bindings = await resolveBindings(jwt, CFG.environmentId);
    if (bindings.length === 0) {
      fail(
        'ninguna instalacion pudo emparejarse con una variable',
        'El simulador deduce la variable del sufijo del serial (T/H/C/N).\n' +
          'Revisa que los seriales sigan ese patron, o ajusta SERIAL_SUFFIX_TO_CODE.',
      );
    }

    const deviceId = await registerDevice(jwt, mac, CFG.environmentId);
    const issued = await requestProvisionToken(jwt, deviceId, mac, bindings);

    log('BLE', '--- aqui la app entregaria el token por BLE; no hay BLE en el simulador ---');
    config = await redeemProvisionToken(issued.token);
  }

  const accessToken = await fetchAccessToken(config);
  console.log('');
  const accepted = await publishSample(config, accessToken);
  console.log('');

  if (!args.includes('--loop')) {
    console.log(`  ${accepted}/${config.bindings.length} lecturas aceptadas.\n`);
    console.log('  Comprobacion en base de datos:');
    console.log('    SELECT measured_at, measured_value');
    console.log('      FROM environment_monitoring.environment_measurement');
    console.log('     ORDER BY measured_at DESC LIMIT 5;\n');
    console.log('  Ojo: las lecturas de la app solo saldran si installation_projection');
    console.log('  esta poblada (infra/iot/populate-installation-projection.sql).\n');
    return;
  }

  console.log(`  Modo --loop: cada ${CFG.sampleIntervalMs} ms. Ctrl-C para salir.\n`);
  for (;;) {
    await new Promise((r) => setTimeout(r, CFG.sampleIntervalMs));
    await publishSample(config, accessToken);
  }
}

main().catch((error) => fail('error inesperado', error.stack ?? String(error)));