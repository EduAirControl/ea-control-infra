#pragma once

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Politica de reintentos del emisor (ADR-021 seccion 6)
//
// - 429: esperar lo que diga Retry-After; si no viene, retroceso exponencial.
// - 5xx, errores de red y demas fallos recuperables: retroceso exponencial.
// Nada se borra del buffer en ninguno de estos casos.
//
// No depende de Arduino: se prueba en el PC (extras/test_retry_policy.cpp).
// ============================================================

// Espera tras el fallo numero `failures` (1 = el primero): base, 2*base, 4*base...
// con tope en capMs. Sin desbordar aunque failures sea enorme.
inline uint32_t backoffDelayMs(uint32_t failures, uint32_t baseMs, uint32_t capMs) {
    if (failures == 0) return 0;
    uint64_t delay = baseMs;
    for (uint32_t i = 1; i < failures && delay < capMs; i++) {
        delay *= 2;
    }
    return delay > capMs ? capMs : (uint32_t)delay;
}

// Resta hasta `percent` % de la espera, segun `random` (cualquier uint32). Reparte a una
// flota de nodos tras una caida del backend para que no reintenten todos a la vez.
// Solo se aplica al retroceso propio: una espera pedida por el servidor (Retry-After)
// se respeta tal cual.
inline uint32_t jitterDown(uint32_t delayMs, uint32_t random, uint32_t percent) {
    if (percent == 0 || delayMs == 0) return delayMs;
    uint64_t span = (uint64_t)delayMs * percent / 100;
    if (span == 0) return delayMs;
    return delayMs - (uint32_t)(random % (span + 1));
}

// Valor de la cabecera Retry-After en milisegundos. Solo se admite la forma de
// segundos enteros ("120"); una fecha HTTP o cualquier otro texto se trata como
// ausente (devuelve 0). Un valor valido se acota a [minMs, maxMs]: 0 s no puede
// producir un bucle sin espera y un valor absurdo no puede dejar al nodo callado horas.
inline uint32_t parseRetryAfterMs(const char* value, uint32_t minMs, uint32_t maxMs) {
    if (value == nullptr) return 0;
    while (*value == ' ' || *value == '\t') value++;
    if (*value < '0' || *value > '9') return 0;

    uint64_t seconds = 0;
    while (*value >= '0' && *value <= '9') {
        seconds = seconds * 10 + (uint32_t)(*value - '0');
        if (seconds > 1000000) seconds = 1000000;  // evita desbordar con digitos de mas
        value++;
    }
    while (*value == ' ' || *value == '\t') value++;
    if (*value != '\0') return 0;  // "1.5", "30s", fechas HTTP...

    uint64_t ms = seconds * 1000;
    if (ms < minMs) ms = minMs;
    if (ms > maxMs) ms = maxMs;
    return (uint32_t)ms;
}

// Estado del retroceso: cuantos fallos seguidos lleva el emisor.
class RetryState {
public:
    // Registra un fallo recuperable y devuelve cuanto esperar antes del siguiente intento.
    // retryAfterMs > 0 manda sobre el retroceso (respuesta 429 con Retry-After).
    uint32_t onFailure(uint32_t retryAfterMs, uint32_t baseMs, uint32_t capMs,
                       uint32_t random, uint32_t jitterPercent) {
        if (failures_ < 0xFFFF) failures_++;
        if (retryAfterMs > 0) return retryAfterMs;
        return jitterDown(backoffDelayMs(failures_, baseMs, capMs), random, jitterPercent);
    }

    void reset() { failures_ = 0; }
    uint32_t failures() const { return failures_; }

private:
    uint32_t failures_ = 0;
};
