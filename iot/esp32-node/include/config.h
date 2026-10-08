#pragma once

// ============================================================
// EduAirControl - nodo sensor ESP32 (arquitectura de microservicios)
//
// TODO lo que hay aqui son valores de COMPILACION, no secretos de produccion:
// son el plan B para una placa recien flasheada y el resultado de pruebas.
// El estado real del nodo vive en NVS y lo entrega el backend al aprovisionar
// (POST /api/v1/auth/device/provision). Ver "Aprovisionamiento sin recompilar"
// en el README.
// ============================================================

#if defined(__has_include)
#if __has_include("config_secrets.h")
#include "config_secrets.h"
#else
#define DEVICE_CLIENT_ID      ""
#define DEVICE_CLIENT_SECRET  ""
#define CA_PEM                ""
#endif
#else
#include "config_secrets.h"
#endif

// ---------- Topologia por defecto (se sobreescribe al aprovisionar) ----------
#define DEFAULT_GATEWAY_HOST  "192.168.1.10"
#define DEFAULT_GATEWAY_PORT  8080
#define DEFAULT_SECURITY_HOST "192.168.1.10"
#define DEFAULT_SECURITY_PORT 8081

#define DEFAULT_CLIENT_ID     "ea-control-device-DEV"
#define DEFAULT_CLIENT_SECRET ""
#define DEFAULT_TOKEN_URL     "http://192.168.1.10:8081/oauth2/token"

// Si USE_HTTPS o CA_PEM vienen de config_secrets.h se respetan; si no, se fijan.
#ifndef USE_HTTPS
#define USE_HTTPS false
#endif

// ---------- Identidad de las variables ----------
// UUIDs CANONICOS de environment_monitoring.variable, sembrados en
// ms-environment-monitoring/.../changes/010-seed-reference.yaml.
// NO son los de ms-sensor-management (esos son gen_random_uuid()).
// El nodo los usa para reconocer que valor va con cada sensorInstallationId.
#define VARIABLE_ID_TEMPERATURE "00000000-0000-4000-8000-000000000071"
#define VARIABLE_ID_HUMIDITY    "00000000-0000-4000-8000-000000000072"
#define VARIABLE_ID_NOISE       "00000000-0000-4000-8000-000000000074"

// ---------- Instalaciones por defecto (solo para pruebas sin backend) ----------
// Vacio en produccion: el backend es quien dice a que instalacion apunta cada
// variable. Se dejan aqui para poder probar el firmware sin levantar nada.
#define INSTALLATION_ID_TEMPERATURE ""
#define INSTALLATION_ID_HUMIDITY    ""
#define INSTALLATION_ID_NOISE       ""

// ---------- Aprovisionamiento BLE ----------
// Coincidir con ea-control-frontend/apps/mobile/.../src/modules/iot/ble/bleProvisioning.js
#define BLE_DEVICE_NAME         "ESP32_Config"
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ---------- Cableado ----------
// Mismo que las capturas de prueba (EduAirControl/iot/esp32/*.txt).
// DHT11: DATA -> GPIO5 con pull-up 4.7k-10k entre DATA y VCC.
#define DHT_PIN                 5
#define DHT_TYPE                DHT11

// KY-038: A0 -> GPIO34 (solo entrada, canal ADC1), VCC -> 5V, pot a media escala.
#define KY038_PIN               34
#define KY038_ADC_SAMPLES       50
#define KY038_SAMPLE_DELAY_MS   2

// Conversion ruido -> dB SPL. APROXIMACION: el KY-038 entrega tension, no dB.
//   util = (desviacion tipica - NOISE_ADC_FLOOR) * NOISE_GAIN
//   dB   = NOISE_DB_MIN + util * (NOISE_DB_MAX - NOISE_DB_MIN) / NOISE_UTIL_FULL_SCALE
// Calibrar contra un sonometro (ver README).
#define NOISE_ADC_FLOOR         200
#define NOISE_GAIN              4
#define NOISE_UTIL_FULL_SCALE   1000
#define NOISE_DB_MIN            30.0f
#define NOISE_DB_MAX            100.0f

// ---------- Temporizacion ----------
// El DHT11 exige >= 1 s entre lecturas; 30 s publica 6 req/min contra el
// gateway, dentro del rate limit de 100/min (RATE_LIMIT_MAX en application.yml).
#define SAMPLE_INTERVAL_MS      30000UL
#define HTTP_TIMEOUT_MS         10000
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_MAX_ATTEMPTS       4

// Renovacion del access token. client_credentials no emite refresh token: el
// nodo pide uno nuevo entero cuando este caduca. Se renueva con margen para no
// perder lecturas por un 401 en mitad de una tanda.
#define TOKEN_RENEW_MARGIN_MS   60000UL

// Timeout del canje del token de aprovisionamiento. La app espera 40 s.
#define PROVISION_TIMEOUT_MS    30000

#define NTP_SERVER              "pool.ntp.org"
#define UTC_OFFSET_SECONDS      -18000  // America/Bogota

// 0 = silencioso .. 4 = verbose
#define LOG_LEVEL               3