#pragma once

// ============================================================
// EduAirControl - SECRETOS del nodo ESP32   (NO SUBIR A GIT)
//
// En produccion esto NO hace falta. Las credenciales llegan por BLE dentro del
// token de aprovisionamiento y el firmware las guarda en NVS. Este archivo solo
// sirve para las placas de pruebas que se queman sin pasar por la app.
//
// Copiar en include/config_secrets.h solo si se va a compilar con credenciales
// fijas; si no, el firmware arranca con los valores por defecto de config.h.
// ============================================================

// Credenciales del cliente OAuth2.
// Valores por defecto de ms-security: client id "ea-control-device-<mac>"
#define DEVICE_CLIENT_ID      "ea-control-device-DEV"
#define DEVICE_CLIENT_SECRET  ""

// PEM de la CA con la que se firma el TLS del gateway y de ms-security.
// Vacio + USE_HTTPS=true -> el firmware cae en setInsecure(), aceptable solo en
// pruebas.
#define CA_PEM                ""