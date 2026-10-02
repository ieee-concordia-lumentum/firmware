#pragma once

#include <stdbool.h>
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#define PROTOCOL_ISR_ATTR IRAM_ATTR
#else
#define PROTOCOL_ISR_ATTR
#endif

#define PROTOCOL_SPEED_KBPS        16U
#define PROTOCOL_PACKET_BYTES      14U
#define PROTOCOL_MAX_MESSAGE_BYTES 256U

#ifdef __cplusplus
extern "C" {
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

