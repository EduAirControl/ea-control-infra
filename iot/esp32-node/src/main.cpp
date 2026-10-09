#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "config.h"

// ============================================================
// EduAirControl - Nodo ESP32 de medicion ambiental
//
// Publica temperatura, humedad, ruido y calidad de aire en
// POST /api/v1/measurements de ms-environment-monitoring.
// El alta, la instalacion y la credencial se hacen desde la app movil:
// este firmware solo mide, provisiona por BLE y envia.
// ============================================================

// ─────────────────────────────────────────────────────────────
// Estado del dispositivo (persistido en NVS)
// ─────────────────────────────────────────────────────────────
static String wifiSsid;
static String wifiPass;
static String deviceToken;
static String installationId;

static Preferences nvs;
static DHT dht(DHT11_PIN, DHT_TYPE);

static unsigned long bootMs = 0;
static unsigned long lastReadMs = 0;
static bool bleRunning = false;

// ─────────────────────────────────────────────────────────────
// Persistencia
// ─────────────────────────────────────────────────────────────
static void loadConfig() {
    nvs.begin(NVS_NAMESPACE, false);
    wifiSsid = nvs.getString("ssid", "");
    wifiPass = nvs.getString("pass", "");
    deviceToken = nvs.getString("token", "");
    installationId = nvs.getString("install", "");
}

static void saveConfig() {
    nvs.putString("ssid", wifiSsid);
    nvs.putString("pass", wifiPass);
    nvs.putString("token", deviceToken);
    nvs.putString("install", installationId);
}

static bool hasCredentials() {
    return wifiSsid.length() > 0 && wifiPass.length() > 0;
}

static bool readyToReport() {
    return hasCredentials() && deviceToken.length() > 0 && installationId.length() > 0;
}

// ─────────────────────────────────────────────────────────────
// BLE provisioning
//
// Contrato (guia_configuracion_esp32_react_native_ble.md):
//   app -> nodo : JSON {"ssid","pass","token","installationId"}
//   nodo -> app : "STATUS:ERR_JSON" | "STATUS:CONNECTED" | "STATUS:FAIL"
// ─────────────────────────────────────────────────────────────
static BLEServer* bleServer = nullptr;
static BLECharacteristic* bleChar = nullptr;

static void notifyStatus(const char* status) {
    if (bleChar == nullptr) return;
    bleChar->setValue(status);
    bleChar->notify();
}

static void stopProvisioning() {
    if (!bleRunning) return;
    BLEDevice::deinit(true);
    bleRunning = false;
    Serial.println("[BLE] provisioning detenido");
}

static void connectWifiFromConfig() {
    Serial.printf("[WIFI] conectando a '%s'\n", wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] conectado, IP %s\n", WiFi.localIP().toString().c_str());
        notifyStatus("STATUS:CONNECTED");
        // Con WiFi ya no hace falta el BLE: se libera la radio.
        stopProvisioning();
    } else {
        Serial.println("[WIFI] no se pudo conectar");
        notifyStatus("STATUS:FAIL");
    }
}

class ServerCallbacks : public BLEServerCallbacks {
    void onDisconnect(BLEServer* server) override {
        if (bleRunning) {
            BLEDevice::startAdvertising();
        }
    }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        String value = String(characteristic->getValue().c_str());
        if (value.length() == 0) return;

        JsonDocument doc;
        if (deserializeJson(doc, value)) {
            Serial.println("[BLE] JSON invalido");
            notifyStatus("STATUS:ERR_JSON");
            return;
        }

        const char* ssid = doc["ssid"];
        const char* pass = doc["pass"];
        const char* token = doc["token"];
        const char* install = doc["installationId"];

        if (ssid == nullptr || pass == nullptr || token == nullptr || install == nullptr) {
            Serial.println("[BLE] faltan campos (ssid, pass, token, installationId)");
            notifyStatus("STATUS:ERR_JSON");
            return;
        }

        wifiSsid = ssid;
        wifiPass = pass;
        deviceToken = token;
        installationId = install;
        saveConfig();
        Serial.println("[BLE] credenciales guardadas en NVS");

        connectWifiFromConfig();
    }
};

static void startProvisioning() {
    if (bleRunning) return;

    BLEDevice::init(BLE_DEVICE_NAME);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
    bleChar = service->createCharacteristic(
            BLE_CHAR_UUID,
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
                    | BLECharacteristic::PROPERTY_NOTIFY);
    bleChar->addDescriptor(new BLE2902());
    bleChar->setCallbacks(new CharacteristicCallbacks());
    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    bleRunning = true;
    Serial.printf("[BLE] esperando provisioning como '%s'\n", BLE_DEVICE_NAME);
}

// ─────────────────────────────────────────────────────────────
// Sensores
// ─────────────────────────────────────────────────────────────

// KY-038: RMS del ADC -> dB aproximados.
static float readNoiseDb() {
    uint32_t sum = 0;
    for (int i = 0; i < KY038_SAMPLES; i++) {
        uint32_t raw = analogRead(KY038_PIN);
        sum += raw * raw;
        delayMicroseconds(KY038_SAMPLE_WINDOW_US);
    }
    float rms = sqrtf((float)sum / (float)KY038_SAMPLES);
    return NOISE_DB_MIN + (rms / 4095.0f) * (NOISE_DB_MAX - NOISE_DB_MIN);
}

// MQ-135: ADC -> voltaje real (con divisor) -> Rs -> ppm aproximado por la
// curva del datasheet. NO es un CO2 NDIR: el valor es orientativo.
static float readCo2Ppm() {
    float raw = (float)analogRead(MQ135_PIN);
    float vOut = (raw / 4095.0f) * 3.3f * MQ135_DIVIDER_RATIO;
    if (vOut <= 0.01f) return NAN;

    float vC = 5.0f;
    float rs = ((vC - vOut) * MQ135_RL) / vOut;
    if (rs <= 0.0f) return NAN;

    float ratio = rs / MQ135_R0;
    return 116.6020682f * powf(ratio, -2.769034857f);
}

// ─────────────────────────────────────────────────────────────
// Ingesta HTTP
// ─────────────────────────────────────────────────────────────
static bool sendMeasurement(const char* variableId, float value) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (isnan(value)) return false;

    JsonDocument doc;
    doc["sensorInstallationId"] = installationId;
    doc["variableId"] = variableId;
    doc["value"] = serialized(String(value, 2));
    // measuredAt se omite a proposito: el backend usa su propio reloj.

    String body;
    serializeJson(doc, body);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    String url = String("http://") + BACKEND_HOST + ":" + String(BACKEND_PORT) + API_PATH;

    if (!http.begin(client, url)) {
        Serial.println("[HTTP] no se pudo abrir la conexion");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + deviceToken);

    int code = http.POST(body);
    http.end();

    bool ok = code == 200 || code == 201;
    Serial.printf("[HTTP] %s -> %d %s\n", variableId, code, ok ? "ok" : "FALLO");
    return ok;
}

static void readAndReport() {
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    float noiseDb = readNoiseDb();
    float co2Ppm = readCo2Ppm();

    Serial.println("---- lectura ----");
    Serial.printf("  temperatura : %s\n", isnan(temperature) ? "n/d" : String(temperature, 1).c_str());
    Serial.printf("  humedad     : %s\n", isnan(humidity) ? "n/d" : String(humidity, 1).c_str());
    Serial.printf("  ruido       : %.1f dB\n", noiseDb);
    Serial.printf("  CO2 (aprox) : %s\n", isnan(co2Ppm) ? "n/d" : String(co2Ppm, 0).c_str());

    digitalWrite(LED_STATUS_PIN, HIGH);
    sendMeasurement(VAR_TEMPERATURE, temperature);
    sendMeasurement(VAR_HUMIDITY, humidity);
    sendMeasurement(VAR_NOISE, noiseDb);
    sendMeasurement(VAR_CO2, co2Ppm);
    digitalWrite(LED_STATUS_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────
// Setup & loop
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("=== EduAirControl - nodo ESP32 ===");

    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);
    pinMode(KY038_PIN, INPUT);
    pinMode(MQ135_PIN, INPUT);
    dht.begin();
    loadConfig();

    bootMs = millis();
    Serial.printf("  instalacion : %s\n", installationId.length() ? installationId.c_str() : "(sin asignar)");
    Serial.printf("  token       : %s\n", deviceToken.length() ? "presente" : "ausente");
    Serial.printf("  backend     : %s:%d\n", BACKEND_HOST, BACKEND_PORT);

    if (hasCredentials()) {
        connectWifiFromConfig();
    } else {
        Serial.println("[BLE] sin credenciales: hay que provisionar");
    }

    // Sin credenciales completas siempre se puede reprovisionar.
    if (!readyToReport()) {
        startProvisioning();
    }

    Serial.printf("[SENSOR] calentando el MQ-135 (%lu s)...\n", MQ135_WARMUP_MS / 1000);
}

void loop() {
    if (bleRunning) {
        // Mientras se provisiona no se mide: evita enviar con credenciales a medias.
        delay(100);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] desconectado, reintentando...");
        WiFi.reconnect();
        delay(5000);
        return;
    }

    if (!readyToReport()) {
        startProvisioning();
        return;
    }

    // El MQ-135 necesita calentar: hasta entonces sus lecturas no valen.
    if (millis() - bootMs < MQ135_WARMUP_MS) {
        unsigned long left = (MQ135_WARMUP_MS - (millis() - bootMs)) / 1000;
        Serial.printf("[SENSOR] calentamiento MQ-135, faltan %lu s\n", left);
        delay(5000);
        return;
    }

    if (millis() - lastReadMs >= SENSOR_READ_INTERVAL_MS) {
        lastReadMs = millis();
        readAndReport();
    }

    delay(100);
}
