#pragma once
#include "protocol.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Each UART record contains a sequence number, the existing ECC frame, and a CRC32.
// COBS encodes the record so zero can mark its end. Numbers are little-endian.
#define UART_LINK_BAUD 460800UL
#define UART_LINK_RAW_BYTES (4U + PROTOCOL_FRAME_BYTES + 4U)
#define UART_LINK_MAX_ENCODED (UART_LINK_RAW_BYTES + UART_LINK_RAW_BYTES / 254U + 1U)
#define UART_LINK_MAX_RECORD (UART_LINK_MAX_ENCODED + 1U)
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t records;
    uint32_t framing_errors;
    uint32_t crc_errors;
    uint32_t sequence_gaps;
} UartLinkStats;

typedef struct {
    uint8_t encoded[UART_LINK_MAX_ENCODED];
    uint8_t decoded[UART_LINK_RAW_BYTES];
    size_t used;
    bool discarding;
    bool ready;
    bool have_sequence;
    uint32_t previous_sequence;
    UartLinkStats stats;
} UartLinkReceiver;

size_t uartLinkEncode(const uint8_t *frame, uint32_t sequence,
                      uint8_t *record, size_t capacity);
void uartLinkInit(UartLinkReceiver *receiver);
bool uartLinkFeed(UartLinkReceiver *receiver, uint8_t byte);
void uartLinkRelease(UartLinkReceiver *receiver);
void uartLinkAbort(UartLinkReceiver *receiver);
const uint8_t *uartLinkFrame(const UartLinkReceiver *receiver);
#ifdef __cplusplus
}
#endif
