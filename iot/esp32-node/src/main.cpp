/*
  EduAirControl - nodo sensor ESP32 (arquitectura de microservicios)
  =================================================================
  Aprovisionamiento por BLE -> configuracion por HTTPS -> telemetria.

  Puesta en marcha SIN recompilar. La app movil entrega por BLE un token de un
  solo uso; el nodo lo canjea por su configuracion completa y la guarda en NVS.
  Cambiar de aula, de secreto o de instalacion ya no obliga a flashear.

  Contratos verificados contra el codigo:

  - App movil  ea-control-frontend/.../src/modules/iot/ble/bleProvisioning.js
      anuncio "ESP32_Config", UUIDs de config.h,
      escribe Base64 de {"ssid":"...","pass":"...","token":"..."},
      espera STATUS:CONNECTED y despues STATUS:PROVISIONED,
      LEE la caracteristica para obtener la MAC (no usa device.id porque en iOS
      es un UUID periferico y el backend exige AA:BB:CC:DD:EE:FF).

  - ms-security DeviceProvisioningController
      POST /api/v1/auth/device/provision   {token}  -> config, sin JWT
        -> {deviceId, macAddress, clientId, clientSecret, tokenUrl,
            gatewayBaseUrl, accessTokenPath, measurementsPath,
            bindings:[{variableId, sensorInstallationId}]}
      El token es de un solo uso: un reenvio responde 409.

  - ms-security OAuth2ServerConfig
      POST /oauth2/token, grant client_credentials, CLIENT_SECRET_BASIC.

  - api-gateway
      Path=/api/v1/measurements/** -> ms-environment-monitoring:3003.
      PublicPaths no incluye esa ruta: exige Bearer. Y hay que pasar SIEMPRE por
      el gateway: ms-environment-monitoring valida HS256 con jwt.secret mientras
      el gateway valida RS256 contra JWKS, asi que un token de ms-security seria
      rechazado si se pega directo a :3003. Lo que lo salva es que su JwtAuthFilter
      prioriza el header X-User-Id que inyecta el gateway.

  - ms-environment-monitoring MeasurementController
      POST /api/v1/measurements -> 201 {"id": "<uuid>"}
      UNA lectura por request: no hay endpoint batch. Este nodo publica 3 por
      muestra.
*/

#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "config.h"

static const char *PROVISION_PATH = "/api/v1/auth/device/provision";

// ---------- NVS ----------
static const char *NVS_NAMESPACE = "eduair";
static const char *NVS_SSID = "ssid";
static const char *NVS_PASS = "pass";
static const char *NVS_PROVISIONED = "provisioned";
static const char *NVS_DEVICE_ID = "deviceId";
static const char *NVS_CLIENT_ID = "clientId";
static const char *NVS_CLIENT_SECRET = "clientSecret";
static const char *NVS_TOKEN_URL = "tokenUrl";
static const char *NVS_SECURITY_URL = "securityUrl";
static const char *NVS_GATEWAY_URL = "gatewayUrl";
static const char *NVS_MEASUREMENTS_PATH = "measurementsPath";
static const char *NVS_BINDINGS = "bindings";

static const uint8_t MAX_BINDINGS = 6;

// ---------- Estado ----------
Preferences prefs;
DHT dht(DHT_PIN, DHT_TYPE);

BLEServer *bleServer = nullptr;
BLECharacteristic *bleCharacteristic = nullptr;
bool bleRunning = false;
bool bleStopping = false;

String pendingSsid;
String pendingPass;
String pendingToken;
bool connectRequested = false;

String savedSsid;
String savedPass;

String accessToken;
unsigned long tokenExpiresAtMs = 0;

unsigned long lastSampleAt = 0;
unsigned long wifiRetryAt = 0;
int wifiAttempts = 0;
bool timeSynced = false;

/**
 * Configuracion efectiva del nodo. Se carga de NVS al arrancar y se reemplaza
 * por completo cuando el token se canjea. Los campos vacios significan "no hay
 * backend todavia" y el firmware cae a los valores compilados de config.h.
 */
struct DeviceConfig
{
  String deviceId;
  String clientId;
  String clientSecret;
  String tokenUrl;
  String securityBaseUrl;
  String gatewayUrl;
  String measurementsPath;
  bool fromBackend = false;
};

DeviceConfig cfg;

/** Vinculo variable canonica -> instalacion que la produce. */
struct Binding
{
  String variableId;
  String sensorInstallationId;
};

Binding bindings[MAX_BINDINGS];
uint8_t bindingCount = 0;

static const uint8_t HISTORY_SIZE = 6;

// Una lectura pendiente = una fila de environment_measurement. El endpoint es
// por lectura, asi que el buffer guarda valores sueltos y no muestras completas.
// Necesita los DOS uuid: el de la instalacion (propio de ms-sensor-management)
// y el canonico de la variable. No son intercambiables.
struct PendingReading
{
  String installationId;
  String variableId;
  float value;
  time_t measuredAt;
};

PendingReading history[HISTORY_SIZE];
uint8_t historyCount = 0;
uint8_t historyHead = 0;

// ============================================================
// Logging
// ============================================================
static void logAt(int level, const char *fmt, ...)
{
  if (LOG_LEVEL < level)
  {
    return;
  }
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
}

#define logInfo(...) logAt(2, __VA_ARGS__)
#define logDebug(...) logAt(3, __VA_ARGS__)

// ============================================================
// Base64 (la app movil codifica con react-native-base64)
// ============================================================
static const char *B64_TBL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Decode(const String &in)
{
  int8_t rev[256];
  memset(rev, -1, sizeof(rev));
  for (int8_t i = 0; i < 64; i++)
  {
    rev[(uint8_t)B64_TBL[i]] = i;
  }
  String out;
  out.reserve(in.length() * 3 / 4 + 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in.length(); i++)
  {
    uint8_t c = (uint8_t)in[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ')
    {
      continue;
    }
    int8_t v = rev[c];
    if (v < 0)
    {
      continue;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      out += (char)((acc >> bits) & 0xFF);
    }
  }
  return out;
}

static String base64Encode(const String &in)
{
  String out;
  out.reserve(((in.length() + 2) / 3) * 4 + 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in.length(); i++)
  {
    acc = (acc << 8) | (uint8_t)in[i];
    bits += 8;
    while (bits >= 6)
    {
      bits -= 6;
      out += B64_TBL[(acc >> bits) & 0x3F];
    }
  }
  if (bits > 0)
  {
    out += B64_TBL[(acc << (6 - bits)) & 0x3F];
  }
  while (out.length() % 4 != 0)
  {
    out += '=';
  }
  return out;
}

// ============================================================
// Utilidades
// ============================================================
static String isoUtc(time_t t)
{
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(buf);
}

static void syncClock()
{
  if (timeSynced)
  {
    return;
  }
  configTime(UTC_OFFSET_SECONDS, 0, NTP_SERVER);
  struct tm tmv;
  if (getLocalTime(&tmv, 15000))
  {
    timeSynced = true;
    logInfo("[NTP] reloj sincronizado: %s", isoUtc(time(nullptr)).c_str());
  }
  else
  {
    logInfo("[NTP] sin NTP; se omitira measuredAt");
  }
}

static String macToString()
{
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           WiFi.macAddress()[0], WiFi.macAddress()[1], WiFi.macAddress()[2],
           WiFi.macAddress()[3], WiFi.macAddress()[4], WiFi.macAddress()[5]);
  return String(buf);
}

// Envoltorio de HTTP con TLS opcional.
class SecureHttp
{
public:
  void begin(const char *url)
  {
    if (USE_HTTPS)
    {
      if (strlen(CA_PEM) > 0)
      {
        clientSecure.setCACert(CA_PEM);
        clientSecure.setTimeout(HTTP_TIMEOUT_MS / 1000);
      }
      else
      {
        clientSecure.setInsecure();  // solo aceptable en pruebas
      }
      http.begin(clientSecure, url);
    }
    else
    {
      http.begin(clientPlain, url);
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
  }

  void addHeader(const char *k, const String &v) { http.addHeader(k, v); }

  int post(const uint8_t *body, size_t len) { return http.POST(body, len); }

  String body() { return http.getString(); }

  bool readJson(JsonDocument &doc) { return deserializeJson(doc, http.getStream()) == DeserializationError::Ok; }

  void end() { http.end(); }

private:
  WiFiClient clientPlain;
  WiFiClientSecure clientSecure;
  HTTPClient http;
};

// ============================================================
// Configuracion persistente
// ============================================================
static void loadConfigFromNvs()
{
  cfg.deviceId = prefs.getString(NVS_DEVICE_ID, "");
  cfg.clientId = prefs.getString(NVS_CLIENT_ID, "");
  cfg.clientSecret = prefs.getString(NVS_CLIENT_SECRET, "");
  cfg.tokenUrl = prefs.getString(NVS_TOKEN_URL, "");
  cfg.securityBaseUrl = prefs.getString(NVS_SECURITY_URL, "");
  cfg.gatewayUrl = prefs.getString(NVS_GATEWAY_URL, "");
  cfg.measurementsPath = prefs.getString(NVS_MEASUREMENTS_PATH, "/api/v1/measurements");

  bindingCount = 0;
  String blob = prefs.getString(NVS_BINDINGS, "");
  if (blob.length() == 0)
  {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, blob) != DeserializationError::Ok)
  {
    logInfo("[NVS] bindings ilegibles; se ignoran");
    return;
  }
  for (JsonObject b : doc.as<JsonArray>())
  {
    if (bindingCount >= MAX_BINDINGS)
    {
      break;
    }
    bindings[bindingCount].variableId = b["variableId"] | "";
    bindings[bindingCount].sensorInstallationId = b["sensorInstallationId"] | "";
    if (bindings[bindingCount].variableId.length() > 0 &&
        bindings[bindingCount].sensorInstallationId.length() > 0)
    {
      bindingCount++;
    }
  }

  // fromBackend solo si hay lo esencial para publicar: credencial y gateway.
  cfg.fromBackend = cfg.clientSecret.length() > 0 && cfg.gatewayUrl.length() > 0 &&
                    cfg.securityBaseUrl.length() > 0;
  logInfo("[NVS] config cargada (device=%s bindings=%u origen=%s)",
          cfg.deviceId.length() ? cfg.deviceId.c_str() : "-", bindingCount,
          cfg.fromBackend ? "backend" : "compilacion");
}

static void persistConfig(const DeviceConfig &incoming, const Binding *incomingBindings,
                          uint8_t count)
{
  cfg = incoming;

  bindingCount = 0;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < count && i < MAX_BINDINGS; i++)
  {
    JsonObject b = arr.add<JsonObject>();
    b["variableId"] = incomingBindings[i].variableId;
    b["sensorInstallationId"] = incomingBindings[i].sensorInstallationId;
    bindings[bindingCount++] = incomingBindings[i];
  }

  prefs.putString(NVS_DEVICE_ID, cfg.deviceId);
  prefs.putString(NVS_CLIENT_ID, cfg.clientId);
  prefs.putString(NVS_CLIENT_SECRET, cfg.clientSecret);
  prefs.putString(NVS_TOKEN_URL, cfg.tokenUrl);
  prefs.putString(NVS_SECURITY_URL, cfg.securityBaseUrl);
  prefs.putString(NVS_GATEWAY_URL, cfg.gatewayUrl);
  prefs.putString(NVS_MEASUREMENTS_PATH, cfg.measurementsPath);
  prefs.putString(NVS_BINDINGS, serializeJson(doc));
  prefs.putBool(NVS_PROVISIONED, true);

  cfg.fromBackend = cfg.clientSecret.length() > 0 && cfg.gatewayUrl.length() > 0;
  logInfo("[NVS] configuracion guardada (%u bindings)", bindingCount);
}

/** Effective values, con caida a config.h mientras no haya backend. */
static String effectiveClientId() { return cfg.fromBackend ? cfg.clientId : String(DEFAULT_CLIENT_ID); }
static String effectiveClientSecret() { return cfg.fromBackend ? cfg.clientSecret : String(DEFAULT_CLIENT_SECRET); }
static String effectiveTokenUrl() { return cfg.fromBackend ? cfg.tokenUrl : String(DEFAULT_TOKEN_URL); }

/**
 * Raiz de ms-security. Sin backend se recorta DEFAULT_TOKEN_URL quitando los
 * DOS ultimos segmentos: la ruta es /oauth2/token, no solo /token.
 */
static String effectiveSecurityBaseUrl()
{
  if (cfg.fromBackend && cfg.securityBaseUrl.length() > 0)
  {
    return cfg.securityBaseUrl;
  }
  String url = DEFAULT_TOKEN_URL;
  int first = url.lastIndexOf('/');
  int second = first > 0 ? url.lastIndexOf('/', first - 1) : -1;
  return second > 0 ? url.substring(0, second) : url;
}
static String effectiveMeasurementsPath() { return cfg.fromBackend ? cfg.measurementsPath : String("/api/v1/measurements"); }

/**
 * URL base del gateway, SIEMPRE con esquema y puerto: postReading la concatena
 * con la ruta, asi que un host suelto produciria una URL sin esquema.
 */
static String effectiveGatewayUrl()
{
  if (cfg.fromBackend)
  {
    return cfg.gatewayUrl;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "%s://%s:%d", USE_HTTPS ? "https" : "http",
           DEFAULT_GATEWAY_HOST, DEFAULT_GATEWAY_PORT);
  return String(buf);
}

static bool credentialsConfigured()
{
  return effectiveClientSecret().length() > 0;
}

// ============================================================
// Aprovisionamiento: canje del token por la configuracion
// ============================================================

/**
 * POST {tokenUrl con ruta /api/v1/auth/device/provision}.
 *
 * El backend responde 409 si el token ya se canjeo (es de un solo uso) y 400 si
 * fallo la firma o vencio. Cualquier otro fallo se reporta como ERR_PROVISION
 * para que la app lo muestre en vez de quedarse esperando.
 */
static bool redeemProvisionToken(const String &token)
{
  logInfo("[PROV] canjeando token de aprovisionamiento");

  // securityBaseUrl lo entrega el backend ya recortado. NO se deduce de tokenUrl:
  // esa ruta tiene DOS segmentos (/oauth2/token) y recortar el ultimo deja un
  // /oauth2 en medio, con lo que el POST caeria en 404.
  String provisionUrl = effectiveSecurityBaseUrl() + PROVISION_PATH;

  char body[512];
  int len = snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token.c_str());
  if (len <= 0 || len >= (int)sizeof(body))
  {
    logInfo("[PROV] token demasiado largo para el buffer");
    return false;
  }

  SecureHttp http;
  http.begin(provisionUrl.c_str());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  int status = http.post((const uint8_t *)body, (size_t)len);

  if (status < 200 || status >= 300)
  {
    logInfo("[PROV] %d en %s: %s", status, provisionUrl.c_str(), http.body().c_str());
    if (status == 409)
    {
      logInfo("[PROV] el token ya se uso; pide uno nuevo desde la app");
    }
    http.end();
    return false;
  }

  JsonDocument doc;
  if (!http.readJson(doc))
  {
    logInfo("[PROV] respuesta no es JSON valido");
    http.end();
    return false;
  }

  DeviceConfig next;
  next.deviceId = doc["deviceId"] | "";
  next.clientId = doc["clientId"] | "";
  next.clientSecret = doc["clientSecret"] | "";
  next.tokenUrl = doc["tokenUrl"] | "";
  next.securityBaseUrl = doc["securityBaseUrl"] | "";
  next.gatewayUrl = doc["gatewayBaseUrl"] | "";
  next.measurementsPath = doc["measurementsPath"] | "/api/v1/measurements";

  Binding parsed[MAX_BINDINGS];
  uint8_t count = 0;
  for (JsonObject b : doc["bindings"].as<JsonArray>())
  {
    if (count >= MAX_BINDINGS)
    {
      break;
    }
    parsed[count].variableId = b["variableId"] | "";
    parsed[count].sensorInstallationId = b["sensorInstallationId"] | "";
    if (parsed[count].variableId.length() > 0 &&
        parsed[count].sensorInstallationId.length() > 0)
    {
      count++;
    }
  }
  http.end();

  if (next.clientSecret.length() == 0 || next.gatewayUrl.length() == 0 ||
      next.securityBaseUrl.length() == 0 || count == 0)
  {
    logInfo("[PROV] config incompleta: falta clientSecret, gateway, securityBaseUrl o bindings");
    return false;
  }

  logInfo("[PROV] gateway=%s tokenUrl=%s", next.gatewayUrl.c_str(), next.tokenUrl.c_str());
  persistConfig(next, parsed, count);
  logInfo("[PROV] aprovisionado con %u variable(s)", count);
  return true;
}

// ============================================================
// OAuth2: client_credentials contra ms-security
// ============================================================
static bool tokenValid()
{
  if (accessToken.length() == 0)
  {
    return false;
  }
  // Resta sin signo: sigue siendo correcta si millis() dio la vuelta.
  return (long)(tokenExpiresAtMs - millis()) > (long)TOKEN_RENEW_MARGIN_MS;
}

static bool fetchAccessToken(bool force)
{
  if (!credentialsConfigured())
  {
    if (force)
    {
      logInfo("[AUTH] sin clientSecret: aprovisiona el nodo desde la app");
    }
    return false;
  }
  if (!force && tokenValid())
  {
    return true;
  }

  const char *body = "grant_type=client_credentials&scope=sensors.write";
  const String &url = effectiveTokenUrl();

  SecureHttp http;
  http.begin(url.c_str());
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");
  // CLIENT_SECRET_BASIC -> Authorization: Basic base64(client_id:client_secret)
  String basic = effectiveClientId() + ":" + effectiveClientSecret();
  http.addHeader("Authorization", "Basic " + base64Encode(basic));

  int status = http.post((const uint8_t *)body, strlen(body));

  if (status < 200 || status >= 300)
  {
    logInfo("[AUTH] %d en /oauth2/token: %s", status, http.body().c_str());
    if (status == 401 || status == 400)
    {
      logInfo("[AUTH] ms-security no conoce el client_id. Reaprovisiona el nodo.");
    }
    http.end();
    accessToken = "";
    return false;
  }

  JsonDocument doc;
  if (!http.readJson(doc))
  {
    logInfo("[AUTH] respuesta de /oauth2/token no es JSON valido");
    http.end();
    return false;
  }

  const char *token = doc["access_token"] | "";
  long expiresIn = doc["expires_in"] | 3600L;
  accessToken = token;
  tokenExpiresAtMs = millis() + (unsigned long)expiresIn * 1000UL;

  logInfo("[AUTH] token obtenido, expira en %ld s", expiresIn);
  http.end();
  return accessToken.length() > 0;
}

// ============================================================
// Sensores
// ============================================================
static float readNoiseRms()
{
  int32_t sum = 0;
  int32_t sumSq = 0;
  const int n = KY038_ADC_SAMPLES;
  for (int i = 0; i < n; i++)
  {
    int v = analogRead(KY038_PIN);
    sum += v;
    sumSq += (int32_t)v * v;
    delay(KY038_SAMPLE_DELAY_MS);
  }
  float mean = (float)sum / n;
  float variance = (float)sumSq / n - mean * mean;
  if (variance < 0.0f)
  {
    variance = 0.0f;
  }
  return sqrtf(variance);
}

static float adcToDb(float rms)
{
  float util = (rms - NOISE_ADC_FLOOR) * NOISE_GAIN;
  if (util < 0.0f)
  {
    util = 0.0f;
  }
  if (util > NOISE_UTIL_FULL_SCALE)
  {
    util = NOISE_UTIL_FULL_SCALE;
  }
  return NOISE_DB_MIN + util * (NOISE_DB_MAX - NOISE_DB_MIN) / NOISE_UTIL_FULL_SCALE;
}

static bool readSample(float &temperature, float &humidity, float &noiseDb, time_t &measuredAt)
{
  temperature = NAN;
  humidity = NAN;
  measuredAt = time(nullptr);

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  float noise = adcToDb(readNoiseRms());

  if (isnan(t) || isnan(h))
  {
    logInfo("[DHT11] lectura invalida: revise el pull-up y el cable DATA en GPIO%d", DHT_PIN);
  }
  else
  {
    temperature = t;
    humidity = h;
  }
  noiseDb = noise;
  return true;  // el KY-038 siempre aporta lectura
}

static void bufferPush(const String &installationId, const String &variableId,
                       float value, time_t measuredAt)
{
  if (historyCount == HISTORY_SIZE)
  {
    logInfo("[BUF] buffer lleno; se descarta la lectura mas antigua");
  }
  else
  {
    historyCount++;
  }
  history[historyHead].installationId = installationId;
  history[historyHead].variableId = variableId;
  history[historyHead].value = value;
  history[historyHead].measuredAt = measuredAt;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
}

// ============================================================
// Publicacion
// ============================================================
static bool postReading(const PendingReading &reading)
{
  if (!fetchAccessToken(false))
  {
    return false;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s%s", effectiveGatewayUrl().c_str(),
           effectiveMeasurementsPath().c_str());

  char body[320];
  int len = snprintf(body, sizeof(body),
                     "{\"sensorInstallationId\":\"%s\",\"variableId\":\"%s\","
                     "\"value\":%.2f,\"measuredAt\":\"%s\"}",
                     reading.installationId.c_str(), reading.variableId.c_str(),
                     (double)reading.value, isoUtc(reading.measuredAt).c_str());
  if (len <= 0 || len >= (int)sizeof(body))
  {
    logInfo("[POST] no se pudo construir el cuerpo de la lectura");
    return false;
  }

  SecureHttp http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int status = http.post((const uint8_t *)body, (size_t)len);

  bool ok = (status >= 200 && status < 300);
  if (!ok)
  {
    logInfo("[POST] %d en %s", status, url);
    logInfo("[POST] cuerpo: %s", http.body().c_str());
    if (status == 401 || status == 403)
    {
      logInfo("[POST] el gateway rechazo el token; se pide uno nuevo en el proximo intento.");
      accessToken = "";  // fuerza renovacion por client_credentials
    }
    if (status == 429)
    {
      logInfo("[POST] 429 = rate limit del gateway (RATE_LIMIT_MAX por ventana).");
    }
  }
  else
  {
    // 201 con {"id": null} es una reingesta: MeasurementService.record devuelve
    // null cuando ya existe la tripla (instalacion, variable, instante). No falla.
    JsonDocument doc;
    if (http.readJson(doc) && (doc["id"] | "").length() == 0)
    {
      logDebug("[POST] 201 idempotente (lectura ya registrada)");
    }
  }

  http.end();
  return ok;
}

static void flushHistory()
{
  while (historyCount > 0)
  {
    uint8_t oldest = (historyHead + HISTORY_SIZE - historyCount) % HISTORY_SIZE;
    if (!postReading(history[oldest]))
    {
      logDebug("[BUF] %u lectura(s) en espera", historyCount);
      return;
    }
    historyCount--;
  }
}

// ============================================================
// Wi-Fi
// ============================================================
static bool wifiConnectBlocking(const String &ssid, const String &pass)
{
  if (ssid.length() == 0)
  {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void wifiReconnectIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiAttempts = 0;
    return;
  }
  unsigned long now = millis();
  if (now < wifiRetryAt || wifiAttempts >= WIFI_MAX_ATTEMPTS || savedSsid.length() == 0)
  {
    return;
  }

  wifiAttempts++;
  wifiRetryAt = now + 10000UL * wifiAttempts;
  logInfo("[WIFI] reintento %d/%d hacia %s", wifiAttempts, WIFI_MAX_ATTEMPTS, savedSsid.c_str());

  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  if (WiFi.status() == WL_CONNECTED)
  {
    logInfo("[WIFI] reconectado");
    wifiAttempts = 0;
    syncClock();
  }
}

// ============================================================
// BLE
// ============================================================
static void bleNotify(const char *status)
{
  if (!bleRunning || bleCharacteristic == nullptr)
  {
    return;
  }
  // Sin delay aqui: bleNotify se invoca desde onWrite, que corre en el contexto
  // del stack BT y un delay largo rompe el enlace.
  bleCharacteristic->setValue((uint8_t *)status, strlen(status));
  bleCharacteristic->notify();
  logDebug("[BLE] notificado %s", status);
}

class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *server) override { logInfo("[BLE] movil conectado"); }

  void onDisconnect(BLEServer *server) override
  {
    if (bleStopping)
    {
      return;  // estamos liberando el stack; reanunciar usaria memoria liberada
    }
    logInfo("[BLE] movil desconectado; se reanuncia");
    BLEDevice::startAdvertising();
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks
{
  /**
   * La app LEE esta caracteristica para obtener la MAC.
   *
   * En iOS react-native-ble-plx devuelve un UUID periferico en device.id y el
   * backend exige AA:BB:CC:DD:EE:FF, asi que sin esto el registro siempre falla
   * con 400 en iOS. El firmware declara su propia MAC y el problema desaparece.
   */
  void onRead(BLECharacteristic *chr) override
  {
    String mac = macToString();
    chr->setValue((uint8_t *)mac.c_str(), mac.length());
  }

  void onWrite(BLECharacteristic *chr) override
  {
    String raw(chr->getValue().c_str());
    if (raw.length() == 0)
    {
      return;
    }

    String json = base64Decode(raw);
    logDebug("[BLE] recibido %d B -> %s", (int)json.length(), json.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok)
    {
      bleNotify("STATUS:ERR_JSON");
      return;
    }

    // La app envia "ssid"/"pass" (bleProvisioning.js:120). "password" es alias.
    const char *ssid = doc["ssid"] | "";
    const char *pass = doc["pass"] | (doc["password"] | "");
    if (ssid[0] == '\0')
    {
      bleNotify("STATUS:ERR_JSON");
      return;
    }
    pendingSsid = ssid;
    pendingPass = pass;
    // El token es opcional: sin el, el firmware se queda con su config compilada.
    pendingToken = doc["token"] | "";
    connectRequested = true;
  }
};

static void bleStart()
{
  if (bleRunning)
  {
    return;
  }
  BLEDevice::init(BLE_DEVICE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);
  bleCharacteristic = service->createCharacteristic(
      BLE_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);
  bleCharacteristic->addDescriptor(new BLE2902());
  bleCharacteristic->setCallbacks(new CharacteristicCallbacks());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  bleRunning = true;
  logInfo("[BLE] anunciando %s", BLE_DEVICE_NAME);
}

static void bleStop()
{
  if (!bleRunning)
  {
    return;
  }
  bleStopping = true;
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);
  bleServer = nullptr;
  bleCharacteristic = nullptr;
  bleRunning = false;
  bleStopping = false;
  logInfo("[BLE] radio apagada");
}

/**
 * BLE -> WiFi -> aprovisionamiento.
 *
 * Notifica CONNECTED en cuanto hay red (lo que espera la app para pasar a la fase
 * siguiente) y PROVISIONED solo cuando el token quedo canjeado y guardado. Si no
 * habia token, se queda en CONNECTED y sigue con la config compilada.
 */
static void handleCredentials()
{
  if (!connectRequested)
  {
    return;
  }
  connectRequested = false;

  String ssid = pendingSsid;
  String pass = pendingPass;
  String token = pendingToken;
  pendingSsid = "";
  pendingPass = "";
  pendingToken = "";

  logInfo("[WIFI] conectando a %s ...", ssid.c_str());
  if (!wifiConnectBlocking(ssid, pass))
  {
    logInfo("[WIFI] fallo la conexion con %s", ssid.c_str());
    bleNotify("STATUS:FAIL");  // el BLE sigue activo para reintentar
    return;
  }

  prefs.putString(NVS_SSID, ssid);
  prefs.putString(NVS_PASS, pass);
  savedSsid = ssid;
  savedPass = pass;
  wifiAttempts = 0;

  logInfo("[WIFI] conectado. IP=%s", WiFi.localIP().toString().c_str());
  syncClock();

  // Margen para que la notificacion llegue al movil (esto corre en el loop, no
  // en el stack BT, asi que el delay es seguro aqui).
  delay(150);
  bleNotify("STATUS:CONNECTED");

  if (token.length() == 0)
  {
    logInfo("[BLE] sin token: se conserva la configuracion existente");
    delay(1500);
    bleStop();
    return;
  }

  bool ok = redeemProvisionToken(token);

  if (ok)
  {
    // El access token se pide con la config nueva, no con la vieja.
    accessToken = "";
    delay(150);
    bleNotify("STATUS:PROVISIONED");
  }
  else
  {
    logInfo("[PROV] fallo el canje; se informa a la app");
    delay(150);
    bleNotify("STATUS:ERR_PROVISION");
  }

  // Margen para que la notificacion llegue antes de tirar el stack BLE.
  delay(1500);
  bleStop();
}

// ============================================================
// Setup / Loop
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();

  logInfo("EduAirControl nodo ESP32 (microservicios)");
  logInfo("MAC %s", macToString().c_str());

  prefs.begin(NVS_NAMESPACE, false);
  loadConfigFromNvs();
  dht.begin();

  if (prefs.getBool(NVS_PROVISIONED, false))
  {
    logInfo("[NVS] nodo aprovisionado: gateway %s", effectiveGatewayUrl().c_str());
  }
  else
  {
    logInfo("[WIFI] nodo sin aprovisionar: use la app movil para enviar SSID y token");
  }

  if (!credentialsConfigured())
  {
    logInfo("[WARN] sin clientSecret. La ingesta dara 401 hasta que la app "
            "aprovisione el nodo.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  savedSsid = prefs.getString(NVS_SSID, "");
  savedPass = prefs.getString(NVS_PASS, "");

  if (savedSsid.length() > 0 && wifiConnectBlocking(savedSsid, savedPass))
  {
    logInfo("[WIFI] conectado con credenciales guardadas (%s)", savedSsid.c_str());
    wifiAttempts = 0;
    syncClock();
  }
  else
  {
    logInfo("[WIFI] sin credenciales o sin red; se activa el aprovisionamiento BLE");
    bleStart();
  }
}

void loop()
{
  handleCredentials();

  if (WiFi.status() != WL_CONNECTED)
  {
    if (!bleRunning && savedSsid.length() > 0 && wifiAttempts >= WIFI_MAX_ATTEMPTS)
    {
      logInfo("[WIFI] red perdida tras %d intentos; reabriendo BLE", WIFI_MAX_ATTEMPTS);
      bleStart();
    }
    else
    {
      wifiReconnectIfNeeded();
    }
    delay(500);
    return;
  }

  unsigned long now = millis();

  if (bleRunning)
  {
    // Se reabrio BLE por perdida de red; si vuelve, el aprovisionamiento ya hizo
    // su funcion y el radio solo gastaria RAM.
    bleStop();
    syncClock();
  }

  // Se pide token en segundo plano para que el primer POST no espere.
  if (historyCount == 0)
  {
    fetchAccessToken(false);
  }

  if (lastSampleAt == 0)
  {
    lastSampleAt = now;
  }
  else if (now - lastSampleAt >= SAMPLE_INTERVAL_MS)
  {
    lastSampleAt = now;

    float temperature;
    float humidity;
    float noiseDb;
    time_t measuredAt;
    readSample(temperature, humidity, noiseDb, measuredAt);

    // Todas las lecturas comparten measuredAt: es lo que permite agruparlas en
    // una misma instantanea en los analisis posteriores.
    if (cfg.fromBackend && bindingCount > 0)
    {
      for (uint8_t i = 0; i < bindingCount; i++)
      {
        const String &vid = bindings[i].variableId;
        float value = NAN;
        if (vid == VARIABLE_ID_TEMPERATURE)
        {
          value = temperature;
        }
        else if (vid == VARIABLE_ID_HUMIDITY)
        {
          value = humidity;
        }
        else if (vid == VARIABLE_ID_NOISE)
        {
          value = noiseDb;
        }
        // Una variable desconocida (p.ej. co2) se omite en vez de mandar un 0:
        // un cero falso dispararia umbrales y alertas.
        if (!isnan(value))
        {
          bufferPush(bindings[i].sensorInstallationId, vid, roundf(value * 10.0f) / 10.0f, measuredAt);
        }
      }
    }
    else
    {
      // Sin aprovisionar: se usan los valores compilados, para pruebas de mesa.
      if (!isnan(temperature) && strlen(INSTALLATION_ID_TEMPERATURE) > 0)
      {
        bufferPush(INSTALLATION_ID_TEMPERATURE, VARIABLE_ID_TEMPERATURE,
                   roundf(temperature * 10.0f) / 10.0f, measuredAt);
      }
      if (!isnan(humidity) && strlen(INSTALLATION_ID_HUMIDITY) > 0)
      {
        bufferPush(INSTALLATION_ID_HUMIDITY, VARIABLE_ID_HUMIDITY,
                   roundf(humidity * 10.0f) / 10.0f, measuredAt);
      }
      if (strlen(INSTALLATION_ID_NOISE) > 0)
      {
        bufferPush(INSTALLATION_ID_NOISE, VARIABLE_ID_NOISE,
                   roundf(noiseDb * 10.0f) / 10.0f, measuredAt);
      }
    }

    logDebug("[SENS] T=%.1f H=%.1f noise=%.1f dB -> %u lectura(s) en cola",
             temperature, humidity, noiseDb, historyCount);
    flushHistory();
  }

  delay(200);
}