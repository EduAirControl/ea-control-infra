#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ============================================================
// Buffer circular de ciclos de medicion (ADR-021)
//
// Un ciclo = una lectura de las cuatro variables, con su hora UTC. El buffer
// guarda los ciclos que aun no se confirmaron y, si se llena, sobrescribe el mas
// viejo. No depende de Arduino: el almacenamiento (archivo LittleFS en la placa,
// RAM en las pruebas del PC) entra por la plantilla Store.
//
// Store debe ofrecer:
//   bool readAt(uint32_t offset, void* buf, size_t len)
//   bool writeAt(uint32_t offset, const void* buf, size_t len)
// ============================================================

// 24 bytes por registro: 4 + 16 + 2 + 2. Sin relleno entre campos.
#pragma pack(push, 1)
struct CycleRecord {
    uint32_t epoch;        // UTC en segundos; 0 = ranura vacia
    float temperature;
    float humidity;
    float noiseDb;
    float co2Ppm;
    uint16_t seq;          // numero de secuencia, crece de a 1 y da vuelta en 65536
    uint16_t crc;          // CRC-16/CCITT de los 22 bytes anteriores
};
#pragma pack(pop)

static_assert(sizeof(CycleRecord) == 24, "CycleRecord debe medir 24 bytes");

inline uint16_t cycleCrc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

inline bool cycleRecordValid(const CycleRecord& r) {
    return r.epoch != 0 &&
           r.crc == cycleCrc16(reinterpret_cast<const uint8_t*>(&r), sizeof(CycleRecord) - 2);
}

template <typename Store>
class CycleRing {
public:
    // capacity: ranuras del buffer. Debe ser menor que 32768 para poder ordenar
    // dos registros por su secuencia aunque esta de la vuelta.
    CycleRing(Store& store, uint32_t capacity) : store_(store), capacity_(capacity) {}

    // Recorre las ranuras para reconstruir el estado tras un reinicio. ackSeq es la
    // secuencia del ultimo ciclo confirmado por el backend (se guarda aparte, en NVS).
    // Devuelve false si el almacenamiento no se pudo leer.
    bool begin(uint16_t ackSeq) {
        ackSeq_ = ackSeq;
        bool any = false;
        uint32_t validCount = 0;
        uint16_t newestSeq = 0;
        uint32_t newestSlot = 0;

        for (uint32_t slot = 0; slot < capacity_; slot++) {
            CycleRecord r;
            if (!store_.readAt(slot * sizeof(CycleRecord), &r, sizeof(r))) return false;
            if (!cycleRecordValid(r)) continue;
            validCount++;
            // El mas nuevo es el que ningun otro valido sigue (distancia modular).
            if (!any || ahead(r.seq, newestSeq)) {
                newestSeq = r.seq;
                newestSlot = slot;
                any = true;
            }
        }

        if (!any) {
            writeSlot_ = 0;
            nextSeq_ = (uint16_t)(ackSeq_ + 1);
            unsent_ = 0;
            readSlot_ = 0;
            return true;
        }

        writeSlot_ = (newestSlot + 1) % capacity_;
        nextSeq_ = (uint16_t)(newestSeq + 1);

        // Sin confirmar = los que van despues de ackSeq, hasta el mas nuevo.
        // Si el ack guardado quedo mas atras que el buffer (se sobrescribio sin guardarlo),
        // todo lo que sobrevive esta sin confirmar.
        uint32_t distance = (uint16_t)(newestSeq - ackSeq_);
        unsent_ = distance <= capacity_ ? distance : validCount;
        // Un hueco (registro corrupto) puede dejar menos registros que la distancia.
        readSlot_ = (writeSlot_ + capacity_ - unsent_) % capacity_;
        return true;
    }

    // Agrega un ciclo. Si el buffer esta lleno de ciclos sin confirmar, se pierde el mas viejo.
    bool append(CycleRecord record) {
        record.seq = nextSeq_;
        record.crc = cycleCrc16(reinterpret_cast<const uint8_t*>(&record), sizeof(CycleRecord) - 2);
        if (!store_.writeAt(writeSlot_ * sizeof(CycleRecord), &record, sizeof(record))) return false;

        writeSlot_ = (writeSlot_ + 1) % capacity_;
        nextSeq_ = (uint16_t)(nextSeq_ + 1);
        if (unsent_ == capacity_) {
            readSlot_ = writeSlot_;       // se sobrescribio el mas viejo
            ackSeq_ = (uint16_t)(nextSeq_ - 1 - capacity_);
            overwritten_++;
        } else {
            unsent_++;
        }
        return true;
    }

    uint32_t unsent() const { return unsent_; }
    uint32_t overwritten() const { return overwritten_; }
    uint16_t ackSeq() const { return ackSeq_; }

    // Copia hasta max ciclos, del mas viejo al mas nuevo, sin darlos por enviados.
    // Un registro ilegible se salta y se cuenta en skipped para que ack() lo cubra.
    size_t peek(CycleRecord* out, size_t max, size_t* skipped = nullptr) {
        size_t count = 0;
        size_t slotsRead = 0;
        size_t skip = 0;
        uint32_t slot = readSlot_;
        while (slotsRead < unsent_ && count < max) {
            CycleRecord r;
            if (store_.readAt(slot * sizeof(CycleRecord), &r, sizeof(r)) && cycleRecordValid(r)) {
                out[count++] = r;
            } else {
                skip++;
            }
            slot = (slot + 1) % capacity_;
            slotsRead++;
        }
        if (skipped) *skipped = skip;
        lastPeekSlots_ = slotsRead;
        return count;
    }

    // Confirma los ciclos devueltos por el ultimo peek() (mas los saltados).
    // Devuelve la secuencia confirmada, para guardarla en NVS.
    uint16_t ackPeeked() {
        uint32_t n = lastPeekSlots_ <= unsent_ ? (uint32_t)lastPeekSlots_ : unsent_;
        readSlot_ = (readSlot_ + n) % capacity_;
        unsent_ -= n;
        ackSeq_ = (uint16_t)(nextSeq_ - 1 - unsent_);
        lastPeekSlots_ = 0;
        return ackSeq_;
    }

    // Descarta todo (otra instalacion). La secuencia continua para no repetir numeros.
    bool clear() {
        CycleRecord empty;
        memset(&empty, 0, sizeof(empty));
        for (uint32_t slot = 0; slot < capacity_; slot++) {
            if (!store_.writeAt(slot * sizeof(CycleRecord), &empty, sizeof(empty))) return false;
        }
        writeSlot_ = 0;
        readSlot_ = 0;
        unsent_ = 0;
        ackSeq_ = (uint16_t)(nextSeq_ - 1);
        return true;
    }

private:
    // a va despues de b si la diferencia modular cae en la mitad "positiva".
    static bool ahead(uint16_t a, uint16_t b) {
        return (uint16_t)(a - b) != 0 && (uint16_t)(a - b) < 32768;
    }

    Store& store_;
    uint32_t capacity_;
    uint32_t writeSlot_ = 0;
    uint32_t readSlot_ = 0;
    uint32_t unsent_ = 0;
    uint32_t overwritten_ = 0;
    size_t lastPeekSlots_ = 0;
    uint16_t nextSeq_ = 1;
    uint16_t ackSeq_ = 0;
};
