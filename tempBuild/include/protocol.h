#pragma once

#include <stdbool.h>
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#define PROTOCOL_ISR_ATTR IRAM_ATTR
#else
#define PROTOCOL_ISR_ATTR
#endif

// Change this value on BOTH boards: milliseconds per transmitted wire byte.
// 1000 = 1 byte/second; 5000 = 1 byte every 5 seconds; 100 = 10 bytes/second.
// Use a positive multiple of 4 ms for exact four-times sampling at 1 MHz.
#define PROTOCOL_BYTE_PERIOD_MS    5000UL
#if PROTOCOL_BYTE_PERIOD_MS == 0 || (PROTOCOL_BYTE_PERIOD_MS % 4UL) != 0
#error "PROTOCOL_BYTE_PERIOD_MS must be a positive multiple of 4"
#endif
#define PROTOCOL_BIT_PERIOD_US     (PROTOCOL_BYTE_PERIOD_MS * 1000ULL / 8ULL)
#define PROTOCOL_PACKET_BYTES      14U
#define PROTOCOL_MAX_MESSAGE_BYTES 256U


#ifdef __cplusplus
extern "C" {
#endif
// Diagnostic snapshots are read from the main loop; no printing in interrupts.
#ifdef RECEIVER
typedef struct{
    uint32_t edges;
    uint32_t errors;
    uint32_t packets;
    uint16_t wireBytes;
    uint8_t lastError;
    uint8_t input;
    uint8_t progress;
    uint8_t headerBits;
    uint16_t length;
    int16_t corrected;
} ProtocolReceiverDebug;
void protocolGetReceiverDebug(ProtocolReceiverDebug *debug);
#endif

void protocolInit(void);
bool protocolTransmit(const uint8_t *data, uint16_t length);
void protocolProcess(void);
void PROTOCOL_ISR_ATTR protocolTransmitTick(void);
void PROTOCOL_ISR_ATTR protocolReceiveTick(bool sample);
bool protocolIsReady(void);
bool protocolIsTransmitting(void);
const uint8_t *protocolGetMessageData(void);
uint16_t protocolGetMessageLength(void);
int16_t protocolGetCorrectedSymbolCount(void);
uint8_t protocolGetReceiverProgress(void);
void protocolResetReceiver(void);
#ifdef __cplusplus
}
#endif

