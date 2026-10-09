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
#define KY038_SAMPLES         100
#define KY038_SAMPLE_WINDOW_US 100

// MQ-135 (calidad de aire, aproxima CO2): A0 a D33 (GPIO33 = ADC1_CH5).
// VCC a 5V (calentador). La salida pasa por divisor de voltaje antes del ADC.
#define MQ135_PIN             33

// LED de estado: el integrado en la placa.
#define LED_STATUS_PIN        2

// ─────────────────────────────────────────────────────────────
// BACKEND (ms-environment-monitoring, puerto 3003)
//
// El unico camino de ingesta es POST /api/v1/measurements. Exige un token
// de dispositivo (roles:["DEVICE"]): los headers X-User-* estan excluidos
// a proposito.
// ─────────────────────────────────────────────────────────────
// IP del PC que corre el stack (interfaces de red de este equipo: 192.168.100.49).
// IMPORTANTE: es una IP por DHCP y puede cambiar. Reserva la IP en el router o
// cambia este valor y reflashea. El ESP32 y el PC tienen que estar en la misma red.
#define BACKEND_HOST        "192.168.100.49"
#define BACKEND_PORT        3003
#define API_PATH            "/api/v1/measurements"

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
#define MQ135_R0                10.0f   // Resistencia en aire limpio (kOhm). MEDIR.
#define MQ135_RL                10.0f   // Resistencia de carga del modulo (kOhm).
#define MQ135_DIVIDER_RATIO     1.0f    // R2/(R1+R2) inverso. 2.0 si el divisor es 10k/10k.
#define MQ135_WARMUP_MS         180000  // 3 min de calentamiento tras cada arranque.

// ─────────────────────────────────────────────────────────────
// KY-038 (ruido): del ADC 0-4095 a dB aproximados.
// ─────────────────────────────────────────────────────────────
#define NOISE_ADC_MIN           0
#define NOISE_ADC_MAX           4095
#define NOISE_DB_MIN            30.0f
#define NOISE_DB_MAX            90.0f

// ─────────────────────────────────────────────────────────────
// TIMING
// ─────────────────────────────────────────────────────────────
#define SENSOR_READ_INTERVAL_MS  30000   // 30 s entre ciclos de medicion
#define HTTP_TIMEOUT_MS          10000
#define WIFI_CONNECT_TIMEOUT_MS  20000
#define BLE_PROVISION_TIMEOUT_MS 300000  // 5 min esperando provisioning

// ─────────────────────────────────────────────────────────────
// BLE PROVISIONING (contrato de guia_configuracion_esp32_react_native_ble.md)
// ─────────────────────────────────────────────────────────────
#define BLE_SERVICE_UUID  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
