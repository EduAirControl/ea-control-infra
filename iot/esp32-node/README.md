# Nodo sensor ESP32 — EduAirControl (microservicios)

Firmware del dispositivo IoT: aprovisiona Wi-Fi por BLE desde la app móvil, lee
DHT11 (temperatura/humedad) y KY-038 (ruido), y publica la telemetría contra
`api-gateway` → `ms-environment-monitoring`.

```
ESP32 ──BLE──> app móvil (SSID / contraseña)
  │
  ├─ POST /oauth2/token ──────────> ms-security :8081   (client_credentials)
  │
  └─ POST /api/v1/measurements ───> api-gateway :8080 ──> ms-environment-monitoring :3003
     Authorization: Bearer <access token>                    (1 lectura por POST)
```

## Por qué dos destinos

| Servicio | Puerto | Para qué |
|---|---|---|
| `ms-security` | 8081 | `POST /oauth2/token`. El gateway **no** enruta `/oauth2/**`; solo lo marca público para su login BFF. |
| `api-gateway` | 8080 | `POST /api/v1/measurements` → `ms-environment-monitoring:3003` |

**Nunca publicar directo contra `:3003`.** `ms-environment-monitoring` valida el
JWT con **HS256** (`JwtService`, `jwt.secret`) mientras el gateway valida **RS256**
contra JWKS. Un token emitido por `ms-security` es RS256 y sería rechazado. Lo que
lo hace funcionar es que `JwtAuthFilter:47-55` prioriza el header `X-User-Id` que
inyecta el gateway, y salta la validación del JWT.

## Archivos

| Ruta | Qué es |
|---|---|
| `platformio.ini` | PlatformIO. Fija `espressif32@6.x` (core 2.x) porque el core 3.x cambia la API BLE que espera la app. |
| `include/config.h` | Topología, UUIDs de variables, cableado, timings. Versionable. |
| `include/config_secrets.example.h` | Plantilla de credenciales. Copiar, no versionar. |
| `src/main.cpp` | Firmware. |
| `tools/simulate-node.mjs` | **Simulador**: doble de prueba de la parte HTTP, para probar sin la placa. |

## Probar sin la placa física

**Un simulador web de ESP32 no es posible.** Web Bluetooth es **solo rol central**
(un navegador conecta a periféricos, no al revés), así que ninguna web page puede
anunciar `ESP32_Config`. Y **Wokwi no simula Bluetooth** (su propia tabla de
features lo marca ❌ para todos los chips), aunque sí WiFi, ADC y GPIO.

Lo que sí cubre `tools/simulate-node.mjs` es **toda la cadena HTTP**, ejecutando
exactamente las mismas llamadas que `src/main.cpp`:

```bash
cd infra/iot/esp32-node
ENVIRONMENT_ID=<uuid-del-ambiente> node tools/simulate-node.mjs
ENVIRONMENT_ID=<uuid-del-ambiente> node tools/simulate-node.mjs --loop   # cada 30 s
```

Salida esperada:

```
[APP]  login como admin@eduaircontrol.com
[APP]  3 binding(s): temperature, humidity, noise
[APP]  client_id=ea-control-device-deadbeef0001
[BLE]  --- aqui la app entregaria el token por BLE ---
[NODO] canjeando token de aprovisionamiento
[NODO]   gateway  http://localhost:8080
[NODO] pidiendo access token (client_credentials)
[NODO]   temperature   23.4 C   -> 201
[NODO]   humidity      55.2 %   -> 201
[NODO]   noise         73.1 dB  -> 201
  3/3 lecturas aceptadas.
```

Emite valores sintéticos donde el firmware leería los sensores. **Lo único que
requiere la placa es BLE y los GPIO**, y se comprueba en un minuto.

## Prerrequisitos

### 1. Levantar ms-security con la clave de firma del aprovisionamiento

```bash
cd infra
# ya viene en .env.example; si falta:
grep -q DEVICE_PROVISION_SIGNING_KEY .env || \
  echo "DEVICE_PROVISION_SIGNING_KEY=$(openssl rand -base64 48 | tr -d '\n')" >> .env
docker compose up -d
```

`DEVICE_PROVISION_SIGNING_KEY` firma los tokens de aprovisionamiento. Sin ella
`ms-security` **arranca igual** (para no tumbar la autenticación de toda la
plataforma) pero el canje responde 500 y lo dice por log.

Prueba de que la firma funciona:

```bash
curl -s -X POST http://localhost:8080/api/v1/auth/device/provision \
  -H 'Content-Type: application/json' -d '{"token":"invalido"}'
# -> {"message":"token con formato invalido","status":400}
```

### 2. Registrar el dispositivo y pedir el token

Lo hace la app móvil. A mano, para pruebas:

```bash
JWT=$(curl -s -X POST http://localhost:8081/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"admin@eduaircontrol.com","password":"EduAirControl1!"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["accessToken"])')

DEVICE=$(curl -s -X POST http://localhost:8080/api/v1/devices \
  -H "Authorization: Bearer $JWT" -H 'Content-Type: application/json' \
  -d "{\"macAddress\":\"DE:AD:BE:EF:00:01\",\"name\":\"Aula 101\",
       \"educationalEnvironmentId\":\"$ENVIRONMENT_ID\"}" | jq -r .deviceId)

curl -s -X POST http://localhost:8081/api/v1/auth/device/provision-tokens \
  -H "Authorization: Bearer $JWT" -H 'Content-Type: application/json' \
  -d "{\"deviceId\":\"$DEVICE\",\"macAddress\":\"DE:AD:BE:EF:00:01\",
       \"bindings\":[{\"variableId\":\"00000000-0000-4000-8000-000000000071\",
                     \"sensorInstallationId\":\"$INSTALACION_TEMP\"}]}" | jq
```

La respuesta trae `clientId`, `clientSecret` y `token` **en un solo disparo**, y
los tres aparecen una única vez.

### 3. El token es de un solo uso

Verificado contra el stack real:

| Intento | Respuesta |
|---|---|
| Primer canje | `200` con la config completa |
| Reenvío del mismo token | `409 "El token de aprovisionamiento ya fue usado"` |
| Token con la firma alterada | `400 "firma del token invalida"` |

El canje se marca con `SET NX` en Redis, que ya usa ms-security para la lista
negra. Es atómico: dos reenvíos simultáneos solo pasan uno.

### 4. Crear sensores, variables e instalaciones

**`ms-sensor-management` arranca vacío.** No tiene seeds: `sensors.variable` y
`sensors.sensor_installation` nacen con 0 filas (`003-create-variable.yaml` usa
`gen_random_uuid()` y no hay changeset de datos). Hay que crearlos por API.

```bash
# Login en ms-security (este servicio NO pide companyCode; eso era del monolito)
TOKEN=$(curl -s -X POST http://localhost:8081/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"admin@eduaircontrol.com","password":"EduAirControl1!"}' | jq -r .token)

GW=http://localhost:8080
AUTH="Authorization: Bearer $TOKEN"

# Ambiente educativo (ms-classroom-management). PageResponse usa .data, no .items.
AMBIENTE=$(curl -s "$GW/api/v1/educational-environments?page=1&limit=1" -H "$AUTH" \
  | jq -r '.data[0].educationalEnvironmentId')
echo "AMBIENTE=$AMBIENTE"

# sensorModelId y sensorStatusId son referencias logicas: en este servicio NO
# existen las tablas sensor_model/sensor_status y solo se validan como UUID
# (decisiones.md del servicio). Se generan aleatorios.
MODEL=$(python3 -c "import uuid;print(uuid.uuid4())")
STATUS=$(python3 -c "import uuid;print(uuid.uuid4())")

# Sensor + instalación. Repetir tres veces: es una por variable, porque
# environment_measurement es (sensor_installation_id, variable_id, measured_at).
for V in TEMP HUM NOISE; do
  SENSOR=$(curl -s -X POST "$GW/api/v1/sensors" -H "$AUTH" -H 'Content-Type: application/json' \
    -d "{\"serialNumber\":\"EA-101-$V\",\"sensorModelId\":\"$MODEL\",\"sensorStatusId\":\"$STATUS\"}" \
    | jq -r .sensorId)
  INST=$(curl -s -X POST "$GW/api/v1/sensor-installations" -H "$AUTH" -H 'Content-Type: application/json' \
    -d "{\"sensorId\":\"$SENSOR\",\"educationalEnvironmentId\":\"$AMBIENTE\"}" \
    | jq -r .sensorInstallationId)
  echo "$V -> sensor=$SENSOR instalacion=$INST"
done
```

Esos `INST` **no se escriben en ningún archivo**: van dentro del token de
aprovisionamiento, y el firmware los recibe en NVS. Es el punto del diseño: la
configuración del nodo llega del backend, no de una compilación.

### 5. `installation_projection` — se llena sola, con una salvedad

**Sin fila en esta proyección el nodo publica y no se ve nada.** El POST devuelve
`201` y la fila se guarda en `environment_measurement`, pero las lecturas hacen
JOIN contra la proyección:

| Consulta | JOIN |
|---|---|
| `latestByEnvironment` (`GET /current`) | sí |
| `history` (`GET /measurements`) | sí |
| `MeasurementAggregationDao` (dashboard y análisis) | sí |

Ahora la puebla `ms-sensor-management`: cada alta o cierre de instalación escribe
un evento (`sensor.installation.created` / `.closed`) en un outbox transaccional y
`ms-environment-monitoring` lo consume. **Una instalación creada después de este
cambio no necesita ningún paso manual.**

Verificación:

```bash
docker compose exec -T postgres-monitoring psql -U monitoring_user \
  -d eduaircontrol_monitoring \
  -c "SELECT sensor_installation_id, removed_at FROM environment_monitoring.installation_projection;"
```

#### La salvedad: instalaciones anteriores

El outbox solo captura cambios **futuros**. Una instalación que ya existía cuando
se desplegó el publicador nunca emitió evento, así que no tiene fila en la
proyección y sus mediciones quedan guardadas pero invisibles.

Reemitir el evento cerrando y reabriendo la instalación:

```bash
curl -X POST "$GW/api/v1/sensor-installations/{id}/remove" -H "$AUTH"
curl -X POST "$GW/api/v1/sensor-installations" -H "$AUTH" -H 'Content-Type: application/json' \
  -d "{\"sensorId\":\"$SID\",\"educationalEnvironmentId\":\"$ENV\",\"installedAt\":\"$NOW\"}"
```

Como alternativa sigue el script manual (solo para una instalación que no se pueda
cerrar y reabrir):

```bash
psql -h localhost -p 5435 -U monitoring_user -d eduaircontrol_monitoring \
     -f infra/iot/populate-installation-projection.sql
```

#### Lo que sigue pendiente

`educational_environment` (la otra réplica local, que da el `environmentTypeId`)
**sigue sin poblarse**: la llenaría `ms-classroom-management` publicando
`EducationalEnvironment*`, y ese servicio todavía no tiene AMQP. Por eso
`/dashboard/summary` muestra `environments: 0` y **las alertas no se evaluan**, aunque
el detalle del ambiente y `/dashboard/series` ya funcionen.

### 6. Los dos UUID que van en el token

Este es el punto más fácil de equivocar:

| Lo que necesita el POST | De dónde sale | Cómo se ve |
|---|---|---|
| `sensorInstallationId` | `ms-sensor-management` → `sensors.sensor_installation` | **UUID aleatorio**, el que imprima el paso 2 |
| `variableId` | `ms-environment-monitoring` → `environment_monitoring.variable` | **Determinista**, ya sembrado |

Los `variableId` canónicos vienen del seed
`ms-environment-monitoring/.../changes/010-seed-reference.yaml`. El firmware los
tiene compilados (`include/config.h`) **solo para reconocer qué valor va con qué
instalación**; los que se envían son los que llegan en el token.

| Variable | UUID |
|---|---|
| `temperature` | `00000000-0000-4000-8000-000000000071` |
| `humidity` | `00000000-0000-4000-8000-000000000072` |
| `co2` | `00000000-0000-4000-8000-000000000073` |
| `noise` | `00000000-0000-4000-8000-000000000074` |

> El `variableId` que devuelve `GET /api/v1/variables` de `ms-sensor-management`
> **no sirve aquí**: es otra tabla con otros UUID (`gen_random_uuid()`, sin seed).
> Los dos servicios_catalogan `variable` de forma independiente y el vínculo es el
> `sensorInstallationId`, no el UUID de variable.

## Compilar y flashear

```bash
cd infra/iot/esp32-node
pio run -e esp32dev                # compila
pio run -e esp32dev -t upload      # flashea
pio device monitor                 # 115200 baud
```

En una placa recién flasheada **no hace falta `config_secrets.h`**: el firmware
arranca con los valores por defecto de `config.h`, anuncia BLE y espera a que la
app le entregue su configuración. Ese archivo solo sirve para pruebas de mesa que
no pasan por la app.

## Cableado

| Módulo | Pin | Notas |
|---|---|---|
| DHT11 VCC | 3V3 | |
| DHT11 DATA | **GPIO5** | Pull-up 4.7k–10k entre DATA y VCC. Sin él `readTemperature()` devuelve NaN. |
| DHT11 GND | GND | |
| KY-038 VCC | 5V | Desde VIN/5V, no 3V3. |
| KY-038 A0 | **GPIO34** | Solo entrada y canal ADC1: correcto para `analogRead`. |
| KY-038 GND | GND | |

## Aprovisionar el Wi-Fi

1. App móvil → **Dispositivos → Aprovisionar**.
2. Escanea (filtra por `SERVICE_UUID`, exige nombre `ESP32_Config`).
3. Elige el **aula destino**: define qué sensores puede publicar el nodo. Sin ella
   el backend responde 409 porque no hay instalación activa que buscar.
4. La app **lee la MAC del firmware** por BLE. No usa `device.id`, que en iOS es
   un UUID periférico y el backend lo rechaza con 400.
5. SSID y contraseña de **2.4 GHz** (el ESP32 no soporta 5 GHz).
6. El nodo confirma `STATUS:CONNECTED`, canjea el token por HTTPS y confirma
   `STATUS:PROVISIONED`.

Queda con su configuración **en NVS**: cambiar de aula, de secreto o de
instalación ya no obliga a recompilar, se reprovisiona desde la app.

### Nota sobre `securityBaseUrl`

El backend entrega la raíz de ms-security **explícitamente** en la respuesta del
canje, y el firmware no la deduce de `tokenUrl`. Es a propósito: la ruta del
token tiene **dos** segmentos (`/oauth2/token`) y recortar el último deja un
`/oauth2` en medio, con lo que el POST caería en 404.

Si la `ConfigResponse` no trae `securityBaseUrl` (contrato viejo), el firmware lo
detecta, no guarda una configuración a medias y lo dice por log.

## Verificar

```
[WIFI] conectado con credenciales guardadas (MiAula_2.4G)
[WIFI] conectado. IP=192.168.1.77
[NTP] reloj sincronizado: 2026-10-07T15:04:05Z
[AUTH] token obtenido, expira en 3600 s
[SENS] T=23.5 H=58.2 noise=40.5 dB -> 3 lecturas
```

`[AUTH] token obtenido` es la señal de que ms-security acepta el cliente.

Verificación del backend (sin placa):

```bash
cd infra/iot/esp32-node
ENVIRONMENT_ID=<uuid> node tools/simulate-node.mjs
```

```sql
SELECT measured_at, measured_value
  FROM environment_monitoring.environment_measurement
 ORDER BY measured_at DESC LIMIT 5;
```

```bash
# Y el servicio debe devolverlas
curl -s "$GATEWAY/api/v1/environments/$AMBIENTE/current" -H "$AUTH" | jq
```

## Ajustes habituales

| Síntoma | Causa |
|---|---|
| `[AUTH] 401 en /oauth2/token` | `DEVICE_OAUTH2_CLIENT_SECRET` no coincide, o ms-security arrancó sin la variable. |
| `[POST] 401/403` | Token expirado sin renovar, o el gateway no valida el JWKS (`AUTH_JWKS_URI` mal apuntado). |
| `[POST] 429` | Rate limit del gateway: `RATE_LIMIT_MAX=100` por `RATE_LIMIT_WINDOW=60` s. 3 POST / 30 s = 6 req/min por nodo → ~16 nodos antes de agotarlo. |
| `[DHT11] lectura invalida` | Pull-up ausente, DATA en otro pin, o VCC del DHT11 a 5V con pull-up a 5V. |
| Ruido siempre en 30 dB | Subir `NOISE_GAIN` o bajar `NOISE_ADC_FLOOR`. |
| Ruido saturado en 100 dB | Bajar `NOISE_GAIN` o el potenciómetro del KY-038. |

### Calibrar el ruido

El KY-038 **no entrega dB SPL**. `adcToDb()` es lineal sobre la desviación típica
de las muestras y por defecto es una aproximación:

1. Silencio real en el aula → anotar el `rms` que imprime `[SENS]`. Ese valor es `NOISE_ADC_FLOOR`.
2. ~70 dB en el sonómetro → anotar `rms` y los dB reales. Con eso se despeja `NOISE_GAIN`.
3. Ajustar `NOISE_UTIL_FULL_SCALE` para que el valor alto coincida, dentro de `NOISE_DB_MIN`/`NOISE_DB_MAX`.

Los umbrales de alerta están en `010-seed-reference.yaml` (65 dB aula, 75 dB
laboratorio). Si la escala está mal, el dashboard miente.

## Límites conocidos

1. **Las instalaciones anteriores al publicador siguen invisibles.** El outbox solo
   captura el futuro; hay que cerrar y reabrir cada instalación previa, o usar el
   script manual. Lo nuevo funciona sin pasos extra.

2. **El módulo IoT de la app móvil no funciona contra microservicios.**
   `deviceService.js` y `ProvisioningScreen.syncBackend()` llaman a
   `POST /api/v1/devices`, que **solo existe en el monolito**. En
   microservicios no hay tabla de dispositivos ni endpoint de registro, así que
   `syncBackend()` dará 404. El aprovisionamiento Wi-Fi por BLE sí funciona
   (no toca backend); lo que falla es el registro y la entrega de credenciales.
   Cerrarlo requiere un endpoint de dispositivos en `ms-sensor-management`.

3. **Credencial compartida, no por dispositivo.** `ea-control-device` es un único
   cliente: todos los nodos comparten `client_secret` y revocar uno revoca todos.
   Para credenciales por nodo hay que registrar un `RegisteredClient` por MAC; el
   firmware ya lo soporta (basta cambiar `DEVICE_CLIENT_ID`/`SECRET`).

4. **El `client_secret` viaja en claro si `USE_HTTPS=false`.** En `POST /oauth2/token`
   va en la cabecera `Authorization: Basic`. En producción ambos endpoints deben
   ser HTTPS con `CA_PEM`; con la CA vacía el firmware cae en `setInsecure()`.

5. **Buffer de 6 lecturas en RAM.** Si el gateway cae más de 3 minutos (6 × 30 s)
   se pierde lo más antiguo. Persistirlo en LittleFS añade desgaste de flash.

6. **BLE se reabre al perder la red** (~1 min, 4 reintentos con backoff) para poder
   cambiar de SSID sin flashear, y se apaga solo cuando la red vuelve.

7. **Sin CO2.** No hay sensor MH-Z19 en este nodo. El catálogo tiene la variable
   `co2` pero no se envía.

8. **Sin MQTT.** `ADR-004` menciona "MQTT o HTTPS"; lo implementado es HTTPS.
   RabbitMQ lo usa el backend por dentro, no el nodo.

9. **Los `variable_id` están duplicados entre servicios.** `sensors.variable`
   (ms-sensor-management, UUID aleatorio, sin seed) y
   `environment_monitoring.variable` (UUID determinista, sembrado) son catálogos
   independientes con los mismos *codes* pero UUID distintos. El firmware usa
   siempre el canónico. Es frágil: si `010-seed-reference.yaml` cambia sus UUID,
   hay que actualizar `include/config.h` o el `POST` seguirá dando 201 pero las
   lecturas no se asociarán a ningún catálogo.