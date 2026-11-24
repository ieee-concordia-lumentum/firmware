// Wrap each error-corrected frame for UART. A zero marks the end of a record so
// the receiver can find the next boundary after a damaged record.

#include "uart_link.h"
#include <string.h>

// Wrap an existing ECC frame for the wire and recover it at the receiver.
// A zero byte ends each record, so the receiver can find the next frame after an error.

static uint32_t recordCrc(const uint8_t *data, size_t length){
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xEDB88320) : 0U);
    }
    return ~crc;
}

static void storeLinkNumber(uint8_t *p, uint32_t value){
    for (unsigned i = 0; i < 4; ++i){
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

static uint32_t readLinkNumber(const uint8_t *p){
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i){
        value |= (uint32_t)p[i] << (8U * i);
    }
    return value;
}
// COBS replaces embedded zeros with run lengths. A full run holds 254 bytes.
size_t uartLinkEncode(const uint8_t *frame, uint32_t sequence,
                      uint8_t *record, size_t capacity) {
    if (!frame || !record || capacity < UART_LINK_MAX_RECORD){
        return 0;
    }
    uint8_t raw[UART_LINK_RAW_BYTES];
    storeLinkNumber(raw, sequence);
    memcpy(raw + 4, frame, PROTOCOL_FRAME_BYTES);
    storeLinkNumber(raw + UART_LINK_RAW_BYTES - 4, recordCrc(raw, UART_LINK_RAW_BYTES - 4));
    size_t output = 1;
    size_t codePosition = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < sizeof(raw); ++i) {
        if (raw[i] != 0) {
            record[output++] = raw[i];
            if (++code < 0xFF) {
                continue;
            }
        }

        record[codePosition] = code;
        codePosition = output++;
        code = 1;
    }
    record[codePosition] = code;
    record[output++] = 0;
    return output;
}

void uartLinkInit(UartLinkReceiver *receiver){
    memset(receiver, 0, sizeof(*receiver));
}

void uartLinkRelease(UartLinkReceiver *receiver){
    receiver->ready = false;
}

void uartLinkAbort(UartLinkReceiver *receiver){
    if (receiver->used){
        ++receiver->stats.framing_errors;
    }
    receiver->used = 0;
    receiver->ready = false;
    receiver->discarding = true;
}

const uint8_t *uartLinkFrame(const UartLinkReceiver *receiver){
    return receiver->decoded + 4;
}

// Rebuild the original bytes from one complete record and check its size.
static bool decodeRecord(UartLinkReceiver *receiver, size_t length){
    size_t input = 0;
    size_t output = 0;
    while (input < length) {
        const uint8_t code = receiver->encoded[input++];
        const size_t count = code - 1U;
        if (count > length - input || count > sizeof(receiver->decoded) - output) {
            return false;
        }
        memcpy(receiver->decoded + output, receiver->encoded + input, count);
        input += count;
        output += count;
        if (code != 0xFF && input < length) {
            if (output == sizeof(receiver->decoded)) {
                return false;
            }
            receiver->decoded[output++] = 0;
        }
    }
    return output == UART_LINK_RAW_BYTES;
}

// Collect one byte at a time. After an oversized record, wait for the next zero.
bool uartLinkFeed(UartLinkReceiver *receiver, uint8_t byte){
    // Caller must consume the frame before feeding further bytes.
    if (receiver->ready){
        return true;
    }
    if (byte != 0) {
        if (receiver->discarding){
            return false;
        }
        if (receiver->used == sizeof(receiver->encoded)) {
            ++receiver->stats.framing_errors;
            receiver->used = 0;
            receiver->discarding = true;
            return false;
        }
        receiver->encoded[receiver->used++] = byte;
        return false;
    }
    if (receiver->discarding) {
        receiver->discarding = false;
        receiver->used = 0;
        return false;
    }
    if (receiver->used == 0){
        return false;
    }

    const size_t length = receiver->used;
    receiver->used = 0;
    if (!decodeRecord(receiver, length)) {
        ++receiver->stats.framing_errors;
        return false;
    }

    const bool crcOk = readLinkNumber(receiver->decoded + UART_LINK_RAW_BYTES - 4) ==
        recordCrc(receiver->decoded, UART_LINK_RAW_BYTES - 4);
    if (!crcOk) {
        // Preserve the corrupted encoded frame for the desktop's ECC/CRC and logs.
        // Do not trust the sequence number if its CRC failed.
        ++receiver->stats.crc_errors;
    } else {
        const uint32_t sequence = readLinkNumber(receiver->decoded);
        if (receiver->have_sequence && sequence != receiver->previous_sequence + 1U)
            ++receiver->stats.sequence_gaps;
        receiver->previous_sequence = sequence;
        receiver->have_sequence = true;
    }
    ++receiver->stats.records;
    receiver->ready = true;
    return true;
}
