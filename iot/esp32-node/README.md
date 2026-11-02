# Nodo ESP32 — EduAirControl

Firmware del nodo de medición ambiental. Lee **temperatura y humedad** (DHT-11),
**ruido** (KY-038) y **calidad de aire** (MQ-135) y las envía al backend en **lotes de un minuto**.
Cada lectura se guarda primero en un buffer en la flash (24 h), así que un corte
de red o de Wi-Fi no pierde datos. La configuración Wi-Fi, el token del dispositivo y su instalación llegan por
**BLE** desde la app móvil: no hay nada que compilar dentro del código.

---

## 1. Qué necesitas

| | |
|---|---|
| Placa | ESP32 DevKit (30 pines) |
| Cable | USB **de datos** (muchos de carga no sirven) |
| PC | Linux/macOS/Windows con [PlatformIO](https://platformio.org/install/cli) |
| Red | Un Wi-Fi con salida a internet (el nodo habla con `api.eduaircontrol.tech` por HTTPS) |
| Backend | `ms-environment-monitoring` desplegado con el endpoint `POST /api/v1/measurements/batch` |

Comprueba que PlatformIO ve tu placa **antes** de flashear:

```bash
pio device list
```

Deberías ver algo como `/dev/ttyUSB0` (chip CH340) o `/dev/ttyACM0` (CP2102).
Si solo ves `/dev/ttyS*` no es tu placa: no está conectada o falta el driver.

En Linux, tu usuario tiene que estar en el grupo `dialout`:

```bash
sudo usermod -aG dialout $USER   # y cierra sesión para que surta efecto
```

---

## 2. Cableado

| Sensor | Señal | Pin en la placa | GPIO | Alimentación |
|--------|-------|-----------------|------|--------------|
| DHT-11 | DATA | `D5` | **GPIO5** | 3V3 |
| DHT-11 | VCC / GND | `3V3` / `GND` | — | 3.3 V |
| KY-038 | A0 | `D34` | **GPIO34** | — |
| KY-038 | `+` / `G` | `3V3` / `GND` | — | 3.3 V |
| MQ-135 | A0 | `D33` | **GPIO33** | — |
| MQ-135 | VCC / GND | `VIN` / `GND` | — | **5 V** |

Notas importantes:

- **Pull-up de 10 kΩ** entre DATA y VCC en el DHT-11. Sin él las lecturas fallan.
- **El MQ-135 se alimenta a 5 V** (es el calentador). Su salida analógica pasa por
  un **divisor de voltaje** antes de entrar al pin: el ADC del ESP32 admite 3.3 V
  como máximo.
- **Los dos canales analógicos van a ADC1** (GPIO33 y GPIO34). Es obligatorio:
  ADC2 no funciona con el Wi-Fi encendido.
- **GND común** entre el MQ-135, el resto de sensores y el ESP32.

---

## 3. Qué configurar antes de flashear

Abre `include/config.h` y revisa:

| Constante | Qué es | Cuándo tocarla |
|-----------|--------|----------------|
| `BACKEND_HOST` / `BACKEND_PORT` / `BACKEND_USE_TLS` | Servidor al que se envía (`api.eduaircontrol.tech`, `443`, TLS) | Solo para apuntar a un backend local: pon la IP del PC, el puerto `3003` y `BACKEND_USE_TLS 0` |
| `SENSOR_READ_INTERVAL_MS` | Segundos entre ciclos de medición (10 s) | Casi nunca. Si lo cambias, `BUFFER_CAPACITY` debe cubrir 24 h |
| `SEND_INTERVAL_MS` / `BATCH_MAX_CYCLES` | Un lote por minuto, hasta 25 ciclos (100 lecturas) por petición | Casi nunca (el backend admite hasta 1000 lecturas por lote) |
| `MQ135_R0` | Resistencia del MQ-135 en aire limpio (kΩ) | **Al poner un sensor nuevo**: hay que medirla |
| `MQ135_DIVIDER_RATIO` | Factor del divisor de voltaje del A0 | Si cambias las resistencias. Con divisor 10k/10k → `2.0` |

**Todo lo demás se configura por BLE desde la app**: no hace falta compilar la
contraseña del Wi-Fi ni el token dentro del firmware.

---

## 4. Flashear

```bash
cd infra/iot/esp32-node

pio run -t upload          # compila, detecta el puerto y flashea
pio device monitor -b 115200   # ver los logs (Ctrl+C para salir)
```

Si no detecta el puerto:

```bash
pio device list                 # ver qué hay
pio run -t upload --upload-port /dev/ttyUSB0   # forzarlo
```

La **primera** vez tarda más: se descarga el toolchain. El nodo usa una tabla de
particiones propia (`partitions.csv`): 3 MB para el programa, la NVS en el mismo
sitio de siempre y **1 MB de LittleFS** para el buffer. No hay OTA (el flasheo es
siempre por cable).

> **Actualizando un nodo que ya estaba provisionado:** flashea con `pio run -t upload`
> **sin** `erase`. La NVS (Wi-Fi, token, instalación) queda intacta, no hace falta
> reprovisionar. La primera vez que arranca, el nodo formatea solo la partición
> LittleFS y crea el archivo del buffer (`[BUFFER] file created`).

Si el flasheo falla con `Failed to connect to ESP32`: mantén pulsado el botón
**BOOT** de la placa mientras empieza la subida, y suéltalo cuando aparezca
`Connecting...`.

---

## 5. Provisionar (primera vez, por BLE)

1. **Enciende el ESP32**. En el monitor serie verás algo como:

   ```
   === EduAirControl - nodo ESP32 ===
     instalacion : (sin asignar)
     token       : ausente
     backend     : 192.168.100.49:3003
   [BLE] sin credenciales: hay que provisionar
   [BLE] esperando provisioning como 'ESP32_Config'
   ```

2. **En la app móvil** → pestaña **Aprovisionamiento**:
   - *Escanear* → debe aparecer **`ESP32_Config`**
   - Elige el modelo (**`ESP32 completo`**) y el ambiente
   - Pulsa *Registrar / Enviar* con el SSID y la contraseña de tu Wi-Fi

   La app da de alta el sensor, lo instala, pide su **token de dispositivo** y te
   los manda todo por BLE.

3. El ESP32 responde **`STATUS:CONNECTED`** y deja de anunciar BLE. Desde ese
   momento mide cada 10 s y envía los datos en un lote por minuto.

### Cambiar de red o de instalación

El BLE **ya no se abre solo** cuando se cae el Wi-Fi: un corte de red es un caso
normal y el nodo sigue midiendo y guardando. Para reprovisionar:

1. **Mantén pulsado el botón BOOT 3 segundos.** En el serie sale
   `[BLE] BOOT held: opening provisioning`.
2. Haz el provisioning desde la app como en el paso 2 (otro SSID, otra instalación).
3. El BLE se cierra solo al terminar o a los **5 minutos** si no se usa.

Mientras el BLE está abierto el nodo **sigue midiendo**. El BLE sí se abre solo si el
nodo no tiene credenciales, o si el backend responde `410` (la instalación ya no
existe).

Qué pasa con los datos pendientes al reprovisionar:

| Caso | Buffer |
|------|--------|
| Misma instalación (solo cambió el Wi-Fi) | Se **conserva** y se envía por la red nueva |
| Instalación distinta | Se **descarta**: eran lecturas de otro lugar |

---

## 6. Qué se ve en el monitor serie

```
[SENSOR] calentando el MQ-135 (180 s)...     <- el MQ-135 necesita calentar
...
---- lectura ----
  temperatura : 23.5
  humedad     : 48.0
  ruido       : 41.0 dB
  CO2 (aprox) : 620
[BUFFER] stored, 3 pending                    <- cada ciclo se guarda primero
...
[SYNC] batch 6 cycle(s), 24 reading(s): accepted=24 duplicates=0 rejected=0
```

Cada minuto sale un lote de unos 6 ciclos (24 lecturas). Una lectura llega al
backend hasta ~60 s después de tomarse, y las alertas se evalúan con ese mismo
retraso (ADR-022).

Y en el frontend (`http://localhost:3000`) → detalle del ambiente → las cuatro
métricas con sus valores.

---

## 7. Problemas frecuentes

| Síntoma | Causa | Qué hacer |
|---------|-------|-----------|
| `[SYNC] batch failed (401)` | Token caducado o ausente | Reprovisionar desde la app. Los ciclos **no se pierden**: quedan en el buffer |
| `[SYNC] batch failed (403)` | El token no es de dispositivo o es de otro sensor | Reprovisionar: la app emite el token nuevo |
| `[SYNC] batch failed (404/500)` | El backend no tiene el endpoint de lotes o falló | El nodo reintenta cada minuto y conserva todo. Revisa que `ms-environment-monitoring` esté desplegado |
| `[SYNC] batch failed (-1)` / `(-11)` | No llega al backend (red, DNS o timeout) | Se reintenta solo. Si persiste, revisa el Wi-Fi |
| `[SYNC] 410: the installation no longer exists` | La instalación se cerró | El BLE se abre solo: reprovisionar desde la app |
| `[BUFFER] full: oldest unsent cycle overwritten` | Más de 24 h sin enviar | Se pierden los ciclos más viejos. Restablece la conexión |
| `[BUFFER] write failed` | Error del sistema de archivos | Repórtalo con el `[BOOT] reset reason` y el heap libre que imprime la línea |
| Tras un reinicio no hay datos 3 min | El MQ-135 calienta tras cada arranque | Normal: es el tiempo de calentamiento. Los pendientes se conservan |
| `STATUS:FAIL` | Wi-Fi incorrecto | Reprovisionar con el SSID/contraseña correctos |
| `STATUS:ERR_JSON` | Payload incompleto | Usa la app: espera `{ssid, pass, token, installationId}` |
| Temperatura `n/d` | Cables o pull-up del DHT-11 | Revisa DATA→GPIO5 y la resistencia de 10 kΩ |
| CO₂ errático o muy bajo | MQ-135 sin calentar / sin calibrar | Espera 24 h la primera vez y mide `MQ135_R0` en aire limpio |
| Timeout en la app al provisionar | El nodo no notifica | Firmware anterior o BLE ocupado: acerca el móvil y reintenta |

---

## 8. Buffer sin conexión (ADR-021)

- **Dónde:** archivo `/cycles.bin` en LittleFS. 8.640 registros de 24 bytes
  (207.360 B), uno por ciclo de 10 s, o sea **24 h**. Cada registro lleva su hora
  UTC, un número de secuencia y un CRC; un registro dañado se descarta.
- **Orden y borrado:** se envía del más viejo al más nuevo y **solo se borra lo que el
  backend confirmó** (`200`). Si se llena, se sobrescribe lo más viejo.
- **Con cola pendiente:** se envían 25 ciclos por petición, con 0,8 s entre una y otra.
- **Hora:** el nodo sincroniza con NTP. Las lecturas previas a la sincronización se
  retienen y se descartan si el nodo se reinicia antes de sincronizar.
- **Respuestas del backend:** `200` confirma; `400`/`413` descartan el lote (no se
  arregla reintentando); `401`/`403`/`429`/`5xx`/red se reintentan; `410` detiene el
  envío y abre el BLE.
- **Reinicio:** el estado se reconstruye al arrancar (`[BUFFER] ready: N cycle(s) pending`).

## 9. Sobre el CO₂

El **MQ-135 no es un sensor NDIR de CO₂**: es un sensor general de calidad de
aire (NH3, NOx, alcohol, benceno, humo). El ppm que publica es una
**aproximación** obtenida de la curva `Rs/R0` del datasheet. Sirve para ver
tendencias y disparar alertas, no para mediciones certificadas.

Para que los valores signifiquen algo hay que **calibrar**: dejar el sensor en
aire limpio unas horas y ajustar `MQ135_R0` en `include/config.h` según la
resistencia medida. Al cambiar de módulo hay que recalibrar.
