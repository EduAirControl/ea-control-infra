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
=== EduAirControl - nodo ESP32 ===
  instalacion : (vacio)
  token       : (vacio)
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
  [HTTP] <variable-id> -> 201 ok   (x4)
  ```

- Verificación en backend: la web (`https://eduaircontrol.tech`) o la app muestran
  los 4 valores del ambiente actualizándose cada ~10 s.

## 6. Troubleshooting

| Sintoma en serie | Causa / accion |
|---|---|
| No anuncia BLE | No se borró la NVS: repetir paso 1 |
| `STATUS:FAIL` / timeout en app | Reintentar cerca del nodo; revisar Bluetooth y permisos de la app |
| `401 FALLO` en los POST | Token no valido para el backend: repetir paso 4 |
| `410 Gone` + "hay que aprovisionar" | La instalacion ya no existe: reprovisionar (paso 4) |
| CO2 absurdo | Firmware sin `MQ135_R0` calibrado: rebuild desde este repo actualizado |
| WiFi no conecta tras provisioning | SSID/pass mal escritos; el nodo vuelve a BLE tras 60 s sin WiFi |

## 7. Alternativa: reprovisionar sin borrar la NVS

El nodo entra en modo BLE solo si no conecta WiFi durante 60 s
(`WIFI_REPROVISION_AFTER_MS`). Para forzarlo sin `erase`, apagar el router o
alejar al nodo. Para un reprovisionamiento "limpio", este `erase` es más confiable.
