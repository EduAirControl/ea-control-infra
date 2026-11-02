// Prueba en PC de la logica del buffer circular (sin Arduino).
//   g++ -std=c++17 -Wall -I include extras/test_cycle_ring.cpp -o /tmp/test_cycle_ring && /tmp/test_cycle_ring
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "cycle_ring.h"

struct RamStore {
    std::vector<uint8_t> data;
    explicit RamStore(size_t size) : data(size, 0) {}
    bool readAt(uint32_t off, void* buf, size_t len) {
        if (off + len > data.size()) return false;
        memcpy(buf, &data[off], len);
        return true;
    }
    bool writeAt(uint32_t off, const void* buf, size_t len) {
        if (off + len > data.size()) return false;
        memcpy(&data[off], buf, len);
        return true;
    }
};

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FALLA linea %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static CycleRecord cycle(uint32_t epoch) {
    CycleRecord r;
    memset(&r, 0, sizeof(r));
    r.epoch = epoch;
    r.temperature = 20.0f + (epoch % 10);
    r.humidity = 50.0f;
    r.noiseDb = 40.0f;
    r.co2Ppm = 600.0f;
    return r;
}

int main() {
    const uint32_t N = 100;

    { // Vacio, agregar, leer del mas viejo al mas nuevo y confirmar
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N);
        CHECK(ring.begin(0)); CHECK(ring.unsent() == 0);
        for (uint32_t i = 1; i <= 10; i++) CHECK(ring.append(cycle(1000 + i)));
        CHECK(ring.unsent() == 10);
        CycleRecord out[4]; size_t sk = 0;
        CHECK(ring.peek(out, 4, &sk) == 4); CHECK(sk == 0);
        CHECK(out[0].epoch == 1001 && out[3].epoch == 1004);
        ring.ackPeeked();
        CHECK(ring.unsent() == 6);
        CHECK(ring.peek(out, 4) == 4 && out[0].epoch == 1005);
        printf("ok: orden y confirmacion\n");
    }
    { // Sin confirmar, peek repetido devuelve lo mismo (no borra hasta ack)
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        for (uint32_t i = 1; i <= 5; i++) ring.append(cycle(2000 + i));
        CycleRecord a[3], b[3];
        ring.peek(a, 3); ring.peek(b, 3);
        CHECK(a[0].epoch == b[0].epoch && ring.unsent() == 5);
        printf("ok: peek no consume\n");
    }
    { // Reinicio: se reconstruye el estado desde las ranuras y el ack guardado
        RamStore s(N * 24);
        uint16_t ack;
        { CycleRing<RamStore> ring(s, N); ring.begin(0);
          for (uint32_t i = 1; i <= 30; i++) ring.append(cycle(3000 + i));
          CycleRecord out[12]; ring.peek(out, 12); ack = ring.ackPeeked(); }
        CycleRing<RamStore> ring2(s, N);
        CHECK(ring2.begin(ack)); CHECK(ring2.unsent() == 18);
        CycleRecord out[2]; CHECK(ring2.peek(out, 2) == 2); CHECK(out[0].epoch == 3013);
        CHECK(ring2.append(cycle(3031))); CHECK(ring2.unsent() == 19);
        printf("ok: reinicio conserva lo no confirmado\n");
    }
    { // Lleno: sobrescribe el mas viejo y conserva los N mas nuevos
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        for (uint32_t i = 1; i <= N + 25; i++) ring.append(cycle(4000 + i));
        CHECK(ring.unsent() == N); CHECK(ring.overwritten() == 25);
        CycleRecord out[1]; ring.peek(out, 1);
        CHECK(out[0].epoch == 4026);       // los 25 primeros se perdieron
        // y tras reiniciar el resultado es el mismo
        CycleRing<RamStore> ring2(s, N); ring2.begin(ring.ackSeq());
        CHECK(ring2.unsent() == N); ring2.peek(out, 1); CHECK(out[0].epoch == 4026);
        printf("ok: sobrescritura del mas viejo\n");
    }
    { // La secuencia da la vuelta en 65536 sin romper el orden
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        uint32_t total = 65536 + 300 + 4;   // multiplo de 10: termina justo tras un envio
        CycleRecord out[1];
        for (uint32_t i = 1; i <= total; i++) {
            ring.append(cycle(5000 + i));
            if (i % 10 == 0) { // se envia lo que hay, como el emisor
                CycleRecord batch[100]; ring.peek(batch, 100); ring.ackPeeked();
            }
        }
        CHECK(ring.unsent() == 0);
        for (uint32_t i = 1; i <= 7; i++) ring.append(cycle(900000 + i));
        CycleRing<RamStore> ring2(s, N); ring2.begin(ring.ackSeq());
        CHECK(ring2.unsent() == 7); ring2.peek(out, 1); CHECK(out[0].epoch == 900001);
        printf("ok: vuelta de la secuencia\n");
    }
    { // Escritura cortada (CRC malo): se ignora ese registro, el resto sigue
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        for (uint32_t i = 1; i <= 6; i++) ring.append(cycle(6000 + i));
        s.data[3 * 24 + 5] ^= 0xFF;   // corrompe el cuarto registro
        CycleRing<RamStore> ring2(s, N); ring2.begin(0);
        CycleRecord out[10]; size_t sk = 0;
        size_t n = ring2.peek(out, 10, &sk);
        CHECK(n == 5 && sk == 1);
        CHECK(out[2].epoch == 6003 && out[3].epoch == 6005);
        ring2.ackPeeked();
        CHECK(ring2.unsent() == 0);
        printf("ok: registro corrupto se salta (%zu validos)\n", n);
    }
    { // Reinicio con ranuras vacias pero un ack previo: la secuencia continua
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(500);
        CHECK(ring.unsent() == 0);
        ring.append(cycle(7001)); CHECK(ring.unsent() == 1);
        CycleRing<RamStore> ring2(s, N); ring2.begin(500);
        CHECK(ring2.unsent() == 1);
        printf("ok: buffer vacio con ack previo\n");
    }
    { // clear() para otra instalacion
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        for (uint32_t i = 1; i <= 8; i++) ring.append(cycle(8000 + i));
        CHECK(ring.clear()); CHECK(ring.unsent() == 0);
        ring.append(cycle(8100));
        CycleRing<RamStore> ring2(s, N); ring2.begin(ring.ackSeq());
        CHECK(ring2.unsent() == 1);
        printf("ok: clear\n");
    }
    { // El buffer se lleno y se sobrescribio sin guardar el ack: tras reiniciar
      // todo lo que sobrevive sigue sin confirmar
        RamStore s(N * 24); CycleRing<RamStore> ring(s, N); ring.begin(0);
        for (uint32_t i = 1; i <= N + 40; i++) ring.append(cycle(9000 + i));
        CycleRing<RamStore> ring2(s, N); ring2.begin(0);      // ack viejo guardado en NVS
        CHECK(ring2.unsent() == N);
        CycleRecord out[1]; ring2.peek(out, 1); CHECK(out[0].epoch == 9041);
        printf("ok: ack obsoleto tras sobrescritura\n");
    }
    { // Capacidad real del ADR: 24 h a 10 s = 8640 registros = 207360 bytes
        CHECK(8640 * sizeof(CycleRecord) == 207360);
        RamStore s(8640 * 24); CycleRing<RamStore> ring(s, 8640); ring.begin(0);
        for (uint32_t i = 1; i <= 8640; i++) ring.append(cycle(100000 + i));
        CHECK(ring.unsent() == 8640 && ring.overwritten() == 0);
        printf("ok: 8640 registros = %zu bytes\n", (size_t)8640 * sizeof(CycleRecord));
    }

    printf(failures ? "\n%d FALLAS\n" : "\nTODO OK\n", failures);
    return failures ? 1 : 0;
}
