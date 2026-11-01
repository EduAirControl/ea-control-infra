#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "config.h"
#include "cycle_ring.h"
#if BACKEND_USE_TLS
#include "ca_cert.h"
#endif

// ============================================================
// EduAirControl - Nodo ESP32 de medicion ambiental
//
// Mide temperatura, humedad, ruido y calidad de aire cada 10 s, guarda cada ciclo
// en un buffer circular de LittleFS (24 h) y lo envia en un lote por minuto a
// POST /api/v1/measurements/batch de ms-environment-monitoring.
// El alta, la instalacion y la credencial se hacen desde la app movil:
// este firmware mide, provisiona por BLE (bajo demanda) y envia.
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
static unsigned long bleOpenedMs = 0;
// El Wi-Fi se mantiene sin bloquear el loop: medir no depende de tener red.
static bool wifiStarted = false;
static bool wifiWasUp = false;
static unsigned long wifiLastTryMs = 0;
// Mientras el callback BLE conecta el Wi-Fi, el loop no toca la radio.
static volatile bool provisioningWifi = false;
// Lo levanta el callback BLE al guardar credenciales; el loop reconcilia el buffer.
static volatile bool installationChanged = false;
// El backend contesta 410 cuando la instalacion fue desvinculada o purgada: ya no
// existe y no volvera a aceptar datos. Se corta el envio para no spamear la API.
static bool installationGone = false;

// ─────────────────────────────────────────────────────────────
// Persistencia
// ─────────────────────────────────────────────────────────────
// Instalacion a la que pertenecen los ciclos del buffer, y ultima secuencia confirmada.
static String bufferInstallation;
static uint16_t bufferAck = 0;

static void loadConfig() {
    nvs.begin(NVS_NAMESPACE, false);
    bufferInstallation = nvs.getString("bufInst", "");
    bufferAck = nvs.getUShort("bufAck", 0);
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
    // Pequena espera: notify() es asincrono; sin esto el STATUS:CONNECTED se
    // pierde al deinit inmediato y la app sufre timeout de 45 s.
    delay(300);
    BLEDevice::deinit(true);
    bleRunning = false;
    Serial.println("[BLE] provisioning detenido");
}

// Señal desde el callback BLE (onWrite corre en la task del stack: llamar
// BLEDevice::deinit dentro de ella congela el sistema). El loop() retira el
// BLE cuando lo ve en alto.
static volatile bool pendingStopBle = false;

static void connectWifiFromConfig() {
    // Un aprovisionamiento nuevo trae un installationId nuevo: el rechazo (410)
    // del anterior deja de valer.
    installationGone = false;
    provisioningWifi = true;
    Serial.printf("[WIFI] conectando a '%s'\n", wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    wifiStarted = true;

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] conectado, IP %s\n", WiFi.localIP().toString().c_str());
        notifyStatus("STATUS:CONNECTED");
        // No deinit aqui (estamos en la task BLE): lo hace el loop().
        pendingStopBle = true;
    } else {
        Serial.println("[WIFI] no se pudo conectar");
        notifyStatus("STATUS:FAIL");
    }
    wifiWasUp = WiFi.status() == WL_CONNECTED;
    wifiLastTryMs = millis();
    provisioningWifi = false;
}

class ServerCallbacks : public BLEServerCallbacks {
    void onDisconnect(BLEServer* server) override {
        if (bleRunning) {
            BLEDevice::startAdvertising();
        }
    }
};

// Buffer de reensamblado BLE: la app escribe el JSON en trozos de ~400 chars
// (el payload con el JWT son ~800 bytes y el long-write de Android al
// Bluedroid falla en la practica). Cada onWrite concatena; se parsea cuando
// ArduinoJson deja de reportar IncompleteInput.
static String bleRxBuffer;

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        String value = String(characteristic->getValue().c_str());
        if (value.length() == 0) return;

        bleRxBuffer += value;
        Serial.printf("[BLE] chunk (%d bytes, buffer=%d)\n", (int)value.length(), (int)bleRxBuffer.length());

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, bleRxBuffer);
        if (err == DeserializationError::IncompleteInput) {
            // Faltan trozos: esperar el siguiente write.
            return;
        }
        if (err) {
            bleRxBuffer = "";
            Serial.println("[BLE] JSON invalido");
            notifyStatus("STATUS:ERR_JSON");
            return;
        }

        // JSON completo y valido.
        bleRxBuffer = "";

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
        installationChanged = true;
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
    bleOpenedMs = millis();
    Serial.printf("[BLE] esperando provisioning como '%s'\n", BLE_DEVICE_NAME);
}

// ─────────────────────────────────────────────────────────────
// Sensores
// ─────────────────────────────────────────────────────────────

// KY-038: RMS de la parte AC del ADC -> dB aproximados.
// El modulo entrega una senal sesgada en DC (~VCC/2): sin restar la media el
// "RMS" es practicamente constante y el rudo no mueve el valor. Se quita el
// sesgo y se mapea en escala log (20*log10), como los dB de verdad.
static float readNoiseDb() {
    uint64_t sum = 0, sumSq = 0;
    for (int i = 0; i < KY038_SAMPLES; i++) {
        uint32_t raw = analogRead(KY038_PIN);
        sum += raw;
        sumSq += (uint64_t)raw * raw;
        delayMicroseconds(KY038_SAMPLE_WINDOW_US);
    }
    double n = (double)KY038_SAMPLES;
    double mean = (double)sum / n;
    double var = (double)sumSq / n - mean * mean;
    if (var < 1.0) var = 1.0;
    float rmsAc = (float)sqrt(var);

    float db = 20.0f * log10f(rmsAc) + NOISE_DB_OFFSET;
    if (db < NOISE_DB_MIN) db = NOISE_DB_MIN;
    if (db > NOISE_DB_MAX) db = NOISE_DB_MAX;
    Serial.printf("[KY-038] rms_ac=%.0f\n", rmsAc);
    return db;
}

// MQ-135: ADC -> voltaje real (con divisor) -> Rs -> ppm aproximado por la
// curva del datasheet. NO es un sensor NDIR de CO2: el valor es orientativo.
static float readCo2Ppm() {
    float raw = (float)analogRead(MQ135_PIN);
    float vOut = (raw / 4095.0f) * 3.3f * MQ135_DIVIDER_RATIO;
    if (vOut <= 0.01f) {
        Serial.printf("[MQ-135] sin lectura valida (ADC=%.0f, Vout=%.3fV): "
                      "revisa cableado A0->GPIO%d, VCC 5V y GND\n",
                      raw, vOut, MQ135_PIN);
        return NAN;
    }

    float vC = 5.0f;
    float rs = ((vC - vOut) * MQ135_RL) / vOut;
    if (rs <= 0.0f) return NAN;

    float ratio = rs / MQ135_R0;
    return 116.6020682f * powf(ratio, -2.769034857f);
}

// ─────────────────────────────────────────────────────────────
// Reloj (ADR-021)
//
// measuredAt = instante UTC de la lectura, en segundos enteros. Antes de la
// primera sincronizacion NTP no hay hora: el ciclo se guarda con su offset de
// millis() y se reconstruye al sincronizar.
// ─────────────────────────────────────────────────────────────
struct Cycle {
    uint32_t readMs;   // millis() al leer, para reconstruir la hora si no habia NTP
    time_t epoch;      // UTC en segundos; 0 = leido sin hora sincronizada
    float temperature;
    float humidity;
    float noiseDb;
    float co2Ppm;
};

static Cycle unsyncedCycles[UNSYNCED_CYCLES_MAX];
static uint8_t unsyncedCount = 0;
static bool ntpStarted = false;
static bool clockWasSynced = false;

static bool clockSynced() {
    return time(nullptr) >= MIN_VALID_EPOCH;
}

// Hora de la lectura actual, o 0 si el reloj aun no esta sincronizado.
static time_t nowEpochOrZero() {
    time_t now = time(nullptr);
    return now >= MIN_VALID_EPOCH ? now : 0;
}

// Hora de un ciclo leido sin NTP, restando al reloj ya sincronizado el tiempo
// transcurrido desde la lectura. La resta sin signo absorbe el desbordamiento de millis().
static time_t epochFromOffset(uint32_t readMs) {
    return time(nullptr) - (time_t)((uint32_t)millis() - readMs) / 1000;
}

static void formatUtc(time_t epoch, char* out, size_t size) {
    struct tm utc;
    gmtime_r(&epoch, &utc);
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

// SNTP sigue reintentando por su cuenta y resincroniza cada hora: basta con
// arrancarlo una vez por arranque, ya con Wi-Fi.
static void ensureNtp() {
    if (!ntpStarted) {
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        ntpStarted = true;
        Serial.println("[NTP] syncing clock");
    }
    if (!clockWasSynced && clockSynced()) {
        clockWasSynced = true;
        char iso[25];
        formatUtc(time(nullptr), iso, sizeof(iso));
        Serial.printf("[NTP] clock synced: %s\n", iso);
    }
}

// ─────────────────────────────────────────────────────────────
// Buffer offline (LittleFS)
//
// Archivo de BUFFER_CAPACITY registros de 24 bytes (ver cycle_ring.h). Cada
// escritura cierra y reabre el archivo: es lo que fuerza a LittleFS a confirmarla,
// y un corte de energia solo puede dejar a medias el registro en curso (su CRC
// lo descarta al arrancar).
// ─────────────────────────────────────────────────────────────
class FileStore {
public:
    bool open() {
        if (!LittleFS.begin(true, "/littlefs", 5, "spiffs")) return false;
        const size_t size = (size_t)BUFFER_CAPACITY * sizeof(CycleRecord);
        bool create = !LittleFS.exists(BUFFER_FILE);
        if (!create) {
            File probe = LittleFS.open(BUFFER_FILE, "r");
            create = !probe || probe.size() != size;
            if (probe) probe.close();
        }
        if (create) {
            File fresh = LittleFS.open(BUFFER_FILE, "w");
            if (!fresh) return false;
            uint8_t zeros[480] = {0};
            for (size_t written = 0; written < size; written += sizeof(zeros)) {
                fresh.write(zeros, sizeof(zeros));
            }
            fresh.close();
            Serial.println("[BUFFER] file created");
        }
        file_ = LittleFS.open(BUFFER_FILE, "r+");
        return (bool)file_;
    }

    bool readAt(uint32_t offset, void* buf, size_t len) {
        return file_.seek(offset) && file_.read((uint8_t*)buf, len) == len;
    }

    bool writeAt(uint32_t offset, const void* buf, size_t len) {
        bool ok = file_.seek(offset) && file_.write((const uint8_t*)buf, len) == len;
        file_.close();
        file_ = LittleFS.open(BUFFER_FILE, "r+");
        return ok && (bool)file_;
    }

private:
    File file_;
};

static FileStore store;
static CycleRing<FileStore> ring(store, BUFFER_CAPACITY);
static bool bufferReady = false;

static void saveAck(uint16_t ack) {
    bufferAck = ack;
    nvs.putUShort("bufAck", ack);
}

// Los ciclos del buffer pertenecen a una instalacion. Si se aprovisiona la misma,
// se conservan; si es otra, son lecturas de otro lugar y se descartan.
static void reconcileBufferInstallation() {
    if (!bufferReady || installationId.length() == 0) return;
    if (bufferInstallation == installationId) return;
    if (bufferInstallation.length() > 0 && ring.unsent() > 0) {
        Serial.printf("[BUFFER] new installation: discarding %u pending cycle(s)\n",
                      (unsigned)ring.unsent());
        ring.clear();
        saveAck(ring.ackSeq());
    }
    bufferInstallation = installationId;
    nvs.putString("bufInst", bufferInstallation);
}

static void bufferCycle(const Cycle& cycle, time_t epoch) {
    if (!bufferReady) {
        Serial.println("[BUFFER] unavailable: cycle lost");
        return;
    }
    CycleRecord record;
    memset(&record, 0, sizeof(record));
    record.epoch = (uint32_t)epoch;
    record.temperature = cycle.temperature;
    record.humidity = cycle.humidity;
    record.noiseDb = cycle.noiseDb;
    record.co2Ppm = cycle.co2Ppm;

    static uint32_t lastOverwritten = 0;
    if (!ring.append(record)) {
        Serial.println("[BUFFER] write failed: cycle lost");
        return;
    }
    if (ring.overwritten() != lastOverwritten) {
        lastOverwritten = ring.overwritten();
        Serial.println("[BUFFER] full: oldest unsent cycle overwritten");
    }
    Serial.printf("[BUFFER] stored, %u pending\n", (unsigned)ring.unsent());
}

// ─────────────────────────────────────────────────────────────
// Envio por lotes
// ─────────────────────────────────────────────────────────────
enum class SendResult {
    Ok,     // el backend proceso el lote: todo item es final (creado, duplicado o rechazado)
    Drop,   // el backend lo rechazo entero y reintentar no lo arregla (400, 413)
    Keep,   // fallo transitorio o del token: se conserva y se reintenta
    Gone    // 410: la instalacion ya no existe
};

static void appendReading(String& body, size_t& items, const char* variableId,
                          float value, const char* measuredAtIso) {
    if (!isfinite(value)) return;
    char item[260];
    snprintf(item, sizeof(item),
             "{\"sensorInstallationId\":\"%s\",\"variableId\":\"%s\",\"value\":%.2f,\"measuredAt\":\"%s\"}",
             installationId.c_str(), variableId, value, measuredAtIso);
    if (items++ > 0) body += ',';
    body += item;
}

static SendResult postBatch(const CycleRecord* cycles, size_t count) {
    String body;
    body.reserve(count * 4 * 200 + 32);
    body = "{\"measurements\":[";
    size_t items = 0;
    char iso[25];
    for (size_t i = 0; i < count; i++) {
        formatUtc((time_t)cycles[i].epoch, iso, sizeof(iso));
        appendReading(body, items, VAR_TEMPERATURE, cycles[i].temperature, iso);
        appendReading(body, items, VAR_HUMIDITY, cycles[i].humidity, iso);
        appendReading(body, items, VAR_NOISE, cycles[i].noiseDb, iso);
        appendReading(body, items, VAR_CO2, cycles[i].co2Ppm, iso);
    }
    body += "]}";

    // Todos los valores eran NaN (sensores sin lectura): no hay nada que enviar.
    if (items == 0) return SendResult::Ok;

#if BACKEND_USE_TLS
    WiFiClientSecure client;
    client.setCACert(ISRG_ROOT_X1_PEM);
#else
    WiFiClient client;
#endif
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    String scheme = BACKEND_USE_TLS ? "https" : "http";
    String url = String(scheme) + "://" + BACKEND_HOST + ":" + String(BACKEND_PORT) + API_PATH;

    if (!http.begin(client, url)) {
        Serial.println("[SYNC] could not open the connection");
        return SendResult::Keep;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + deviceToken);

    digitalWrite(LED_STATUS_PIN, HIGH);
    int code = http.POST(body);
    String response = code == 200 ? http.getString() : String();
    http.end();
    digitalWrite(LED_STATUS_PIN, LOW);

    if (code == 200) {
        JsonDocument filter;
        filter["accepted"] = true;
        filter["duplicates"] = true;
        filter["rejected"] = true;
        JsonDocument summary;
        if (!deserializeJson(summary, response, DeserializationOption::Filter(filter))) {
            Serial.printf("[SYNC] batch %u cycle(s), %u reading(s): accepted=%d duplicates=%d rejected=%d\n",
                          (unsigned)count, (unsigned)items, (int)summary["accepted"],
                          (int)summary["duplicates"], (int)summary["rejected"]);
        } else {
            Serial.printf("[SYNC] batch %u cycle(s) -> 200\n", (unsigned)count);
        }
        return SendResult::Ok;
    }
    // 410 Gone: la instalacion ya no existe (desvinculada o purgada). Reenviar no la
    // va a recrear: se corta el envio y se pide reprovisionar.
    if (code == 410) {
        Serial.println("[SYNC] 410: the installation no longer exists, provisioning needed");
        return SendResult::Gone;
    }
    // 400 y 413 describen al lote, no a la red: reintentarlo igual fallaria siempre y
    // bloquearia la cola. Los demas codigos (401, 403, 429, 5xx, red) se reintentan.
    if (code == 400 || code == 413) {
        Serial.printf("[SYNC] batch rejected with %d: dropped\n", code);
        return SendResult::Drop;
    }
    Serial.printf("[SYNC] batch failed (%d): kept for retry, %u pending\n", code,
                  (unsigned)ring.unsent());
    return SendResult::Keep;
}

static unsigned long nextSendMs = 0;

// Un lote por llamada. Mientras quede cola, el siguiente sale tras BATCH_PACING_MS; si
// no, tras SEND_INTERVAL_MS. Solo se borra del buffer lo que el backend confirmo.
static void drainBuffer() {
    if ((long)(millis() - nextSendMs) < 0) return;
    if (!bufferReady || installationGone || WiFi.status() != WL_CONNECTED || !clockSynced()) return;
    if (ring.unsent() == 0) {
        nextSendMs = millis() + SEND_INTERVAL_MS;
        return;
    }

    CycleRecord batch[BATCH_MAX_CYCLES];
    size_t skipped = 0;
    size_t count = ring.peek(batch, BATCH_MAX_CYCLES, &skipped);
    SendResult result = count == 0 ? SendResult::Ok : postBatch(batch, count);

    switch (result) {
        case SendResult::Ok:
        case SendResult::Drop:
            saveAck(ring.ackPeeked());
            nextSendMs = millis() + (ring.unsent() > 0 ? BATCH_PACING_MS : SEND_INTERVAL_MS);
            break;
        case SendResult::Gone:
            installationGone = true;
            break;
        case SendResult::Keep:
            nextSendMs = millis() + SEND_INTERVAL_MS;
            break;
    }
}

// Los ciclos leidos antes de la primera sincronizacion NTP pasan al buffer con la
// hora ya reconstruida.
static void flushUnsyncedCycles() {
    if (unsyncedCount == 0 || !clockSynced()) return;
    Serial.printf("[NTP] rebuilding time of %u earlier cycle(s)\n", unsyncedCount);
    for (uint8_t i = 0; i < unsyncedCount; i++) {
        bufferCycle(unsyncedCycles[i], epochFromOffset(unsyncedCycles[i].readMs));
    }
    unsyncedCount = 0;
}

static void readAndReport() {
    // La hora se toma antes de leer los sensores: es el instante de la lectura.
    Cycle cycle;
    cycle.readMs = millis();
    cycle.epoch = nowEpochOrZero();
    cycle.temperature = dht.readTemperature();
    cycle.humidity = dht.readHumidity();
    cycle.noiseDb = readNoiseDb();
    cycle.co2Ppm = readCo2Ppm();

    Serial.println("---- lectura ----");
    Serial.printf("  temperatura : %s\n", isnan(cycle.temperature) ? "n/d" : String(cycle.temperature, 1).c_str());
    Serial.printf("  humedad     : %s\n", isnan(cycle.humidity) ? "n/d" : String(cycle.humidity, 1).c_str());
    Serial.printf("  ruido       : %.1f dB\n", cycle.noiseDb);
    Serial.printf("  CO2 (aprox) : %s\n", isnan(cycle.co2Ppm) ? "n/d" : String(cycle.co2Ppm, 0).c_str());

    if (cycle.epoch == 0) {
        // Sin hora no se guarda nada con fecha adivinada: se retiene el ciclo con
        // su offset. Si la RAM se llena se descarta el mas viejo.
        if (unsyncedCount == UNSYNCED_CYCLES_MAX) {
            memmove(&unsyncedCycles[0], &unsyncedCycles[1], sizeof(Cycle) * (UNSYNCED_CYCLES_MAX - 1));
            unsyncedCount--;
        }
        unsyncedCycles[unsyncedCount++] = cycle;
        Serial.printf("[NTP] clock not synced: cycle held (%u)\n", unsyncedCount);
        return;
    }

    bufferCycle(cycle, cycle.epoch);
}

// ─────────────────────────────────────────────────────────────
// Wi-Fi y boton BOOT
// ─────────────────────────────────────────────────────────────

// Mantiene la conexion sin bloquear: medir y guardar no dependen de tener red.
static void maintainWifi() {
    if (provisioningWifi || !hasCredentials()) return;

    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !wifiWasUp) {
        Serial.printf("[WIFI] conectado, IP %s\n", WiFi.localIP().toString().c_str());
    } else if (!up && wifiWasUp) {
        Serial.printf("[WIFI] connection lost, measuring continues (%u pending)\n",
                      (unsigned)ring.unsent());
    }
    wifiWasUp = up;
    if (up) return;

    if (!wifiStarted) {
        Serial.printf("[WIFI] conectando a '%s'\n", wifiSsid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        wifiStarted = true;
        wifiLastTryMs = millis();
    } else if (millis() - wifiLastTryMs >= WIFI_RETRY_MS) {
        wifiLastTryMs = millis();
        WiFi.reconnect();
    }
}

// BLE bajo demanda (ADR-021): BOOT pulsado BOOT_HOLD_MS abre el provisioning.
static void handleBootButton() {
    static unsigned long downSince = 0;
    if (digitalRead(BOOT_BUTTON_PIN) != LOW) {
        downSince = 0;
        return;
    }
    if (downSince == 0) {
        downSince = millis();
    } else if (!bleRunning && millis() - downSince >= BOOT_HOLD_MS) {
        Serial.println("[BLE] BOOT held: opening provisioning");
        startProvisioning();
    }
}

// Un nodo ya provisionado cierra el BLE a los 5 min; sin provisionar (o dado de baja)
// lo mantiene abierto, porque no hay otra cosa que pueda hacer.
static void handleBleTimeout() {
    if (bleRunning && readyToReport() && !installationGone &&
        millis() - bleOpenedMs >= BLE_PROVISION_TIMEOUT_MS) {
        Serial.println("[BLE] timeout: closing provisioning");
        stopProvisioning();
    }
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
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    dht.begin();
    loadConfig();

    bufferReady = store.open() && ring.begin(bufferAck);
    if (bufferReady) {
        Serial.printf("[BUFFER] ready: %u cycle(s) pending, capacity %u\n",
                      (unsigned)ring.unsent(), (unsigned)BUFFER_CAPACITY);
        reconcileBufferInstallation();
    } else {
        Serial.println("[BUFFER] could not open LittleFS: readings will not be stored");
    }

    bootMs = millis();
    Serial.printf("  instalacion : %s\n", installationId.length() ? installationId.c_str() : "(sin asignar)");
    Serial.printf("  token       : %s\n", deviceToken.length() ? "presente" : "ausente");
    Serial.printf("  backend     : %s:%d\n", BACKEND_HOST, BACKEND_PORT);

    // El BLE solo se abre solo cuando no hay nada que hacer sin el: primer
    // aprovisionamiento. Con credenciales guardadas el nodo mide aunque no haya
    // Wi-Fi, y el BLE se abre con el boton BOOT.
    if (!readyToReport()) {
        Serial.println("[BLE] sin credenciales: hay que provisionar");
        startProvisioning();
    }

    Serial.printf("[SENSOR] calentando el MQ-135 (%lu s)...\n", MQ135_WARMUP_MS / 1000);
}

void loop() {
    // Retiro del BLE pedido desde el callback: se hace aqui (task principal),
    // nunca dentro de la task del stack BLE.
    if (pendingStopBle) {
        pendingStopBle = false;
        stopProvisioning();
    }

    handleBootButton();
    handleBleTimeout();
    maintainWifi();

    if (installationChanged) {
        installationChanged = false;
        reconcileBufferInstallation();
    }

    if (WiFi.status() == WL_CONNECTED) {
        ensureNtp();
    }
    flushUnsyncedCycles();

    if (!readyToReport()) {
        startProvisioning();
        delay(100);
        return;
    }

    // El backend rechazo la instalacion (410): ya no existe. Se ofrece el BLE para
    // reprovisionar desde la app, sin reflashear.
    if (installationGone) {
        if (!bleRunning) {
            Serial.println("[SENSOR] instalacion dada de baja: entrando en provisioning");
            startProvisioning();
        }
        delay(100);
        return;
    }

    // El MQ-135 necesita calentar: hasta entonces sus lecturas no valen.
    if (millis() - bootMs < MQ135_WARMUP_MS) {
        static unsigned long lastWarmupLog = 0;
        if (millis() - lastWarmupLog >= 30000) {
            lastWarmupLog = millis();
            Serial.printf("[SENSOR] calentamiento MQ-135, faltan %lu s\n",
                          (MQ135_WARMUP_MS - (millis() - bootMs)) / 1000);
        }
        delay(100);
        return;
    }

    if (millis() - lastReadMs >= SENSOR_READ_INTERVAL_MS) {
        lastReadMs = millis();
        readAndReport();
    }

    drainBuffer();
    delay(50);
}
