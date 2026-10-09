# Nodo ESP32 — EduAirControl

Firmware del nodo de medición ambiental. Lee **temperatura y humedad** (DHT-11),
**ruido** (KY-038) y **calidad de aire** (MQ-135) y las publica en el backend.
La configuración Wi-Fi, el token del dispositivo y su instalación llegan por
**BLE** desde la app móvil: no hay nada que compilar dentro del código.

---

## 1. Qué necesitas

| | |
|---|---|
| Placa | ESP32 DevKit (30 pines) |
| Cable | USB **de datos** (muchos de carga no sirven) |
| PC | Linux/macOS/Windows con [PlatformIO](https://platformio.org/install/cli) |
| Red | Tu PC y el ESP32 en la **misma red Wi-Fi** |
| Backend | El stack levantado desde `infra/` (`./up.sh`) |

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
| `BACKEND_HOST` | IP del PC que corre `infra/` | **Siempre** si tu IP cambió. Mira la tuya con `ip addr` |
| `BACKEND_PORT` | Puerto del ingest (`3003`) | Casi nunca |
| `MQ135_R0` | Resistencia del MQ-135 en aire limpio (kΩ) | **Al poner un sensor nuevo**: hay que medirla |
| `MQ135_DIVIDER_RATIO` | Factor del divisor de voltaje del A0 | Si cambias las resistencias. Con divisor 10k/10k → `2.0` |

> ⚠️ Tu IP es **dinámica** (`192.168.100.49` hoy, pero puede cambiar). Lo más
> cómodo es reservarla en el router (DHCP reservation) para que no se mueva.

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

La **primera** vez tarda más: se descarga el toolchain y el esquema de
particiones cambia (usa `min_spiffs`, que deja ~1.9 MB para el programa; el
esquema por defecto se queda corto porque BLE + Wi-Fi + JSON ocupan ~1.6 MB).

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
   momento mide cada 30 s y envía los datos.

Para cambiar de red o de instalación, **borra la configuración** y vuelve a
provisionar (se guarda en NVS):

```cpp
// o reinstala el firmware, que también la borra
```

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
[HTTP] 00000000-0000-4000-8000-000000000071 -> 201 ok
[HTTP] 00000000-0000-4000-8000-000000000072 -> 201 ok
[HTTP] 00000000-0000-4000-8000-000000000074 -> 201 ok
[HTTP] 00000000-0000-4000-8000-000000000073 -> 201 ok
```

Y en el frontend (`http://localhost:3000`) → detalle del ambiente → las cuatro
métricas con sus valores.

---

## 7. Problemas frecuentes

| Síntoma | Causa | Qué hacer |
|---------|-------|-----------|
| `... -> 401` al enviar | Token caducado o ausente | Reprovisionar desde la app |
| `... -> 403` | El token no es de dispositivo | Reprovisionar: la app emite el token nuevo |
| `... -> 000 FALLO` | No llega al backend | Comprueba que `BACKEND_HOST` es tu IP actual y que el stack está levantado (`cd infra && ./up.sh`) |
| `STATUS:FAIL` | Wi-Fi incorrecto | Reprovisionar con el SSID/contraseña correctos |
| `STATUS:ERR_JSON` | Payload incompleto | Usa la app: espera `{ssid, pass, token, installationId}` |
| Temperatura `n/d` | Cables o pull-up del DHT-11 | Revisa DATA→GPIO5 y la resistencia de 10 kΩ |
| CO₂ errático o muy bajo | MQ-135 sin calentar / sin calibrar | Espera 24 h la primera vez y mide `MQ135_R0` en aire limpio |
| Timeout en la app al provisionar | El nodo no notifica | Firmware anterior o BLE ocupado: acerca el móvil y reintenta |

---

## 8. Sobre el CO₂

El **MQ-135 no es un sensor NDIR de CO₂**: es un sensor general de calidad de
aire (NH3, NOx, alcohol, benceno, humo). El ppm que publica es una
**aproximación** obtenida de la curva `Rs/R0` del datasheet. Sirve para ver
tendencias y disparar alertas, no para mediciones certificadas.

Para que los valores signifiquen algo hay que **calibrar**: dejar el sensor en
aire limpio unas horas y ajustar `MQ135_R0` en `include/config.h` según la
resistencia medida. Al cambiar de módulo hay que recalibrar.
