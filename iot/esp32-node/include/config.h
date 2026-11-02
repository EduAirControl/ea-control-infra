#pragma once

// ============================================================
// EduAirControl - Nodo ESP32 de medicion ambiental
//
// Placa: ESP32 DevKit (30 pines). Cableado real verificado por el usuario.
// Los dos canales analogicos van a ADC1: ADC2 no funciona con WiFi activo.
// ============================================================

// ─────────────────────────────────────────────────────────────
// PINOUT HARDWARE (etiquetas de la placa entre parentesis)
// ─────────────────────────────────────────────────────────────

// DHT-11: DATA a D5 (GPIO5). VCC a 3V3, GND comun.
// GPIO5 es digital, asi que no le afecta ser ADC2. Con pull-up de 10k
// queda en reposo alto, que es lo que exige su pin de strapping.
#define DHT11_PIN             5
#define DHT_TYPE              DHT11

// KY-038 (ruido): A0 a D34 (GPIO34 = ADC1_CH6). VCC a 3V3.
// GPIO34 es de solo entrada y no tiene pull-up interno: aqui no hace falta.
#define KY038_PIN             34
#define KY038_SAMPLES         1000    // ~100 ms de ventana (100 us por muestra)
#define KY038_SAMPLE_WINDOW_US 100

// MQ-135 (calidad de aire, aproxima CO2): A0 a D33 (GPIO33 = ADC1_CH5).
// VCC a 5V (calentador). La salida pasa por divisor de voltaje antes del ADC.
#define MQ135_PIN             33

// LED de estado: el integrado en la placa.
#define LED_STATUS_PIN        2

// ─────────────────────────────────────────────────────────────
// BACKEND (api-gateway en la nube: https://api.eduaircontrol.tech)
//
// El nodo envia por lotes a POST /api/v1/measurements/batch. Exige un token
// de dispositivo (roles:["DEVICE"]): los headers X-User-* estan excluidos
// a proposito. El gateway enruta /api/v1/measurements/** a
// ms-environment-monitoring.
// ─────────────────────────────────────────────────────────────
// Entorno de pruebas en la nube (Hostinger, eduaircontrol.tech). TLS con el
// root CA de Let's Encrypt incluido en ca_cert.h.
#define BACKEND_HOST        "api.eduaircontrol.tech"
#define BACKEND_PORT        443
#define BACKEND_USE_TLS     1
#define API_PATH            "/api/v1/measurements/batch"

// ─────────────────────────────────────────────────────────────
// IDENTIDAD DEL DISPOSITIVO
//
// deviceToken e installationId los escribe la app movil por BLE durante el
// provisioning y se guardan en NVS. No se compilados en el firmware.
// ─────────────────────────────────────────────────────────────
#define BLE_DEVICE_NAME     "ESP32_Config"
#define NVS_NAMESPACE       "eduair"

// ─────────────────────────────────────────────────────────────
// VARIABLES MEDIDAS
//
// UUIDs de environment_monitoring.variable (semilla 010-seed-reference.yaml).
// Un mismo nodo publica las cuatro: el DHT-11 da temperatura y humedad.
// ─────────────────────────────────────────────────────────────
#define VAR_TEMPERATURE     "00000000-0000-4000-8000-000000000071"
#define VAR_HUMIDITY        "00000000-0000-4000-8000-000000000072"
#define VAR_CO2             "00000000-0000-4000-8000-000000000073"
#define VAR_NOISE           "00000000-0000-4000-8000-000000000074"

// ─────────────────────────────────────────────────────────────
// CALIBRACION DEL MQ-135
//
// El MQ-135 no es un sensor NDIR de CO2: es un sensor general de calidad de
// aire. El ppm que publicamos es una APROXIMACION obtenida de la curva
// Rs/R0 del datasheet. Para que valga algo hay que medir R0 en aire limpio
// y ajustarla aqui. Sin calentar ~24h la primera vez, las lecturas no valen.
// ─────────────────────────────────────────────────────────────
#define MQ135_R0                271.40f // Medido 2026-10-24 al aire libre: Rs/R0 a 400 ppm.
#define MQ135_RL                10.0f   // Resistencia de carga del modulo (kOhm).
#define MQ135_DIVIDER_RATIO     1.0f    // R2/(R1+R2) inverso. 2.0 si el divisor es 10k/10k.
#define MQ135_WARMUP_MS         180000  // 3 min de calentamiento tras cada arranque.

// ─────────────────────────────────────────────────────────────
// KY-038 (ruido): RMS AC (con sesgo DC restado) -> dB aproximados en escala log.
// db = 20*log10(rms_ac) + OFFSET, con piso NOISE_DB_MIN y techo NOISE_DB_MAX.
// OFFSET se afina empiricalmente con el sensor frente a ruido conocido.
#define NOISE_DB_MIN            30.0f
#define NOISE_DB_MAX            90.0f
#define NOISE_DB_OFFSET         5.0f    // Afino 2026-10-24: reposo~35, aplausos~65

// ─────────────────────────────────────────────────────────────
// TIMING
// ─────────────────────────────────────────────────────────────
// 10 s por ciclo: el DHT-11 exige >=1 s entre lecturas. Bajar de 10 s no gana
// nada en la nube.
#define SENSOR_READ_INTERVAL_MS  10000   // 10 s entre ciclos de medicion
#define HTTP_TIMEOUT_MS          10000
#define WIFI_CONNECT_TIMEOUT_MS  20000   // espera maxima al provisionar por BLE
#define WIFI_RETRY_MS            10000   // reintento de reconexion en marcha
#define BLE_PROVISION_TIMEOUT_MS 300000  // 5 min abierto (si el nodo ya esta provisionado)

// BLE bajo demanda (ADR-021): mantener BOOT (GPIO0) pulsado este tiempo, ya con el
// nodo en marcha, abre el provisioning. No se abre solo al caerse el Wi-Fi.
#define BOOT_BUTTON_PIN          0
#define BOOT_HOLD_MS             3000

// ─────────────────────────────────────────────────────────────
// BUFFER OFFLINE Y ENVIO POR LOTES (ADR-021, ADR-022)
//
// Cada ciclo se guarda primero en un archivo circular de LittleFS y un emisor lo
// vacia una vez por minuto en un lote (6 ciclos = 24 lecturas). Online u offline
// es el mismo camino: una caida solo hace el lote mas grande.
// ─────────────────────────────────────────────────────────────
#define BUFFER_FILE              "/cycles.bin"
#define BUFFER_CAPACITY          8640    // 24 h a un ciclo cada 10 s
#define SEND_INTERVAL_MS         60000   // un lote por minuto
// Tope por peticion: 25 ciclos = 100 lecturas (~20 KB de cuerpo). El contrato admite
// 1000, pero la RAM del nodo no.
#define BATCH_MAX_CYCLES         25
// Vaciando una cola grande se espacian los lotes (el gateway admite 100 peticiones
// por minuto por usuario y ruta). Los reintentos con 429 llegan con el Issue #8.
#define BATCH_PACING_MS          800

// ─────────────────────────────────────────────────────────────
// RELOJ (ADR-021)
//
// El nodo no tiene RTC: su hora sale de NTP. measuredAt es el instante UTC de
// la LECTURA, no del POST, para que un reintento lleve la misma marca y el
// backend pueda deduplicar por (instalacion, variable, measured_at).
// ─────────────────────────────────────────────────────────────
#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.google.com"
#define NTP_SERVER_3        "time.cloudflare.com"
// Sin NTP el reloj del ESP32 arranca en 1970: cualquier epoch menor a esto
// (2024-01-01) se trata como "hora no sincronizada".
#define MIN_VALID_EPOCH     1704067200L
// Ciclos leidos antes de la primera sincronizacion NTP, retenidos en RAM con su
// offset de millis() hasta poder reconstruir su hora. Si el nodo se reinicia
// antes de sincronizar se pierden: nunca se envian con una hora inventada.
// Al sincronizar pasan al buffer persistente (LittleFS) con su hora ya reconstruida.
#define UNSYNCED_CYCLES_MAX 16

// ─────────────────────────────────────────────────────────────
// BLE PROVISIONING (contrato de guia_configuracion_esp32_react_native_ble.md)
// ─────────────────────────────────────────────────────────────
#define BLE_SERVICE_UUID  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
