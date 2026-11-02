# Guía: reflash completo del ESP32 + prueba de provisioning por BLE

Objetivo: borrar la NVS para que el nodo arranque "virgen", anuncie BLE y la app móvil
lo configure desde cero — el flujo real de usuario.

> [!WARNING]
> Al terminar la prueba el nodo queda vinculado a la instalación creada desde la app.
> El `installationId` anterior queda huérfano en el backend (inofensivo; se purga luego
> si se quiere). La **calibración del MQ-135 no se pierde**: `MQ135_R0` va compilado
> en `include/config.h`.

## 0. Requisitos

- ESP32 conectado por USB (`/dev/ttyUSB0`).
- Un solo programa usando el puerto serie (cerrar monitores previos).
- App móvil instalada, con sesión y Bluetooth+permisos activos.
- SSID y contraseña del WiFi donde va el nodo.

## 1. Borrado completo (NVS + particiones)

```bash
cd infra/iot/esp32-node
pio run -t erase
```

Esperar `Chip erase completed successfully`. Esto sí borra WiFi/token/installation
(a diferencia de un reflasheo normal, que preserva la NVS).

> Si el nodo **ya estaba provisionado** y solo actualizas el firmware, **omite este
> paso**: la tabla de particiones nueva deja la NVS en el mismo sitio y los datos se
> conservan. Un `erase` además descarta el buffer de lecturas pendientes.

## 2. Flashear el firmware real

```bash
pio run -t upload
```

Sale `[SUCCESS]` y el board resetea solo.

## 3. Monitor serie (opcional pero recomendado)

```bash
pio device monitor -b 115200
```

Salida esperada:

```
[BOOT] reset reason: 1
=== EduAirControl - nodo ESP32 ===
  instalacion : (vacio)
  token       : (vacio)
[BUFFER] file created
[BLE] iniciando provisioning...
```

## 4. Provisioning desde la app (BLE)

1. En la app, ir al **ambiente destino** (p. ej. 209) → detalle → flujo de
   **aprovisionar nodo/sensor** (crear sensor → instalar → enviar datos por BLE).
2. Seleccionar el dispositivo **`ESP32_Config`** en el escáner BLE
   (service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`).
3. La app envía el JSON `{"ssid","pass","token","installationId"}` por la
   characteristic BLE (`beb5483e-36e1-4688-b7f5-ea07361b26a8`).
4. En el serie debe aparecer:

   ```
   [BLE] provisioning detenido
   [WIFI] conectando a '<ssid>'
   [WIFI] conectado, IP ...
   ```

5. La app debe recibir `STATUS:CONNECTED`. Si da timeout de 45 s, reintentar:
   el firmware espera 300 ms antes de apagar BLE para que el notify llegue.

## 5. Post-provisioning (automático)

- `[SENSOR] calentando el MQ-135 (180 s)` — 3 minutos sin CO2
  (temperatura/humedad/ruido sí salen al terminar).
- Luego, cada 10 s:

  ```
  ---- lectura ----
    temperatura : ...
    CO2 (aprox) : ~350-450 al aire limpio
  [BUFFER] stored, 3 pending
  ```

- Cada minuto, un lote con los ciclos acumulados:

  ```
  [SYNC] batch 6 cycle(s), 24 reading(s): accepted=24 duplicates=0 rejected=0
  ```

- Verificación en backend: la web (`https://eduaircontrol.tech`) o la app muestran
  los 4 valores del ambiente, con hasta ~1 min de retraso.

## 6. Troubleshooting

| Sintoma en serie | Causa / accion |
|---|---|
| No anuncia BLE | No se borró la NVS: repetir paso 1 |
| `STATUS:FAIL` / timeout en app | Reintentar cerca del nodo; revisar Bluetooth y permisos de la app |
| `[SYNC] batch failed (401)` | Token no valido para el backend: repetir paso 4. Las lecturas quedan en el buffer |
| `[SYNC] 410: the installation no longer exists` | La instalacion ya no existe: el BLE se abre solo, reprovisionar (paso 4) |
| CO2 absurdo | Firmware sin `MQ135_R0` calibrado: rebuild desde este repo actualizado |
| WiFi no conecta tras provisioning | SSID/pass mal escritos: mantener BOOT 3 s para abrir el BLE y reenviarlos. El nodo ya no abre el BLE solo por perder el Wi-Fi |

## 7. Reprovisionar sin borrar la NVS (botón BOOT)

Con el nodo ya provisionado, **mantén pulsado BOOT (GPIO0) 3 segundos**. En el serie
sale `[BLE] BOOT held: opening provisioning` y aparece `ESP32_Config`. Repite el paso 4.

- El BLE queda abierto **5 minutos** y se cierra solo; el nodo **sigue midiendo**.
- Misma instalación → conserva las lecturas pendientes. Instalación distinta → las
  descarta.
- Ya no hace falta apagar el router ni usar `erase` para cambiar de red.
