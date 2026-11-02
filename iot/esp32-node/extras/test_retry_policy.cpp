// Prueba en el PC de la politica de reintentos (include/retry_policy.h).
//   g++ -std=c++17 -Wall -I include extras/test_retry_policy.cpp -o /tmp/test_retry && /tmp/test_retry
#include <cstdio>
#include <cstdlib>
#include "retry_policy.h"

static int failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FALLA linea %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main() {
    const uint32_t BASE = 60000, CAP = 600000;

    // Retroceso exponencial: 60, 120, 240, 480 y luego tope de 600 s.
    CHECK(backoffDelayMs(0, BASE, CAP) == 0);
    CHECK(backoffDelayMs(1, BASE, CAP) == 60000);
    CHECK(backoffDelayMs(2, BASE, CAP) == 120000);
    CHECK(backoffDelayMs(3, BASE, CAP) == 240000);
    CHECK(backoffDelayMs(4, BASE, CAP) == 480000);
    CHECK(backoffDelayMs(5, BASE, CAP) == 600000);
    CHECK(backoffDelayMs(6, BASE, CAP) == 600000);
    CHECK(backoffDelayMs(1000000, BASE, CAP) == 600000);   // sin desbordar
    CHECK(backoffDelayMs(3, BASE, 100000) == 100000);       // tope menor que la serie

    // Jitter: solo resta, nunca pasa del porcentaje y nunca deja la espera en 0.
    CHECK(jitterDown(100000, 0, 20) == 100000);
    CHECK(jitterDown(100000, 20000, 20) == 80000);          // maximo descuento
    CHECK(jitterDown(100000, 20001, 20) == 100000);         // da la vuelta (modulo 20001)
    CHECK(jitterDown(100000, 20002, 20) == 99999);
    for (uint32_t r = 0; r < 100000; r += 7) {
        uint32_t d = jitterDown(100000, r, 20);
        CHECK(d >= 80000 && d <= 100000);
    }
    CHECK(jitterDown(100000, 123456, 0) == 100000);
    CHECK(jitterDown(0, 5, 20) == 0);
    CHECK(jitterDown(3, 5, 20) == 3);                        // span 0: sin cambio

    // Retry-After: segundos enteros, acotado a [1 s, 1 h].
    const uint32_t MIN = 1000, MAX = 3600000;
    CHECK(parseRetryAfterMs("30", MIN, MAX) == 30000);
    CHECK(parseRetryAfterMs("  5  ", MIN, MAX) == 5000);
    CHECK(parseRetryAfterMs("0", MIN, MAX) == 1000);         // 0 s no es "sin espera"
    CHECK(parseRetryAfterMs("99999", MIN, MAX) == 3600000);  // tope de 1 h
    CHECK(parseRetryAfterMs("99999999999999999999", MIN, MAX) == 3600000);
    CHECK(parseRetryAfterMs(nullptr, MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("", MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("abc", MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("-5", MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("1.5", MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("30s", MIN, MAX) == 0);
    CHECK(parseRetryAfterMs("Wed, 21 Oct 2026 07:28:00 GMT", MIN, MAX) == 0);

    // Estado: la secuencia crece, Retry-After manda, reset vuelve al inicio.
    RetryState s;
    CHECK(s.onFailure(0, BASE, CAP, 0, 0) == 60000 && s.failures() == 1);
    CHECK(s.onFailure(0, BASE, CAP, 0, 0) == 120000);
    CHECK(s.onFailure(45000, BASE, CAP, 0, 0) == 45000);     // 429 con Retry-After
    CHECK(s.onFailure(0, BASE, CAP, 0, 0) == 480000);        // el 429 cuenta como fallo
    s.reset();
    CHECK(s.failures() == 0);
    CHECK(s.onFailure(0, BASE, CAP, 0, 0) == 60000);
    for (int i = 0; i < 100000; i++) s.onFailure(0, BASE, CAP, 0, 0);
    CHECK(s.onFailure(0, BASE, CAP, 0, 0) == 600000);        // el contador no desborda

    if (failed == 0) std::printf("TODO OK\n");
    return failed == 0 ? 0 : 1;
}
