#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
    #include <esp_attr.h>
    #define PROTOCOL_ISR_ATTR IRAM_ATTR
#else
    #define PROTOCOL_ISR_ATTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

void protocolInit(void);
void protocolTransmit(const char *message);
void protocolProcess(void);

void PROTOCOL_ISR_ATTR protocolTransmitTick(void);
void PROTOCOL_ISR_ATTR protocolReceiveTick(bool sample);

bool protocolIsReady(void);
bool protocolIsTransmitting(void);
uint8_t protocolGetReceiverProgress(void);
uint16_t protocolGetMessageLength(void);
int8_t protocolGetCorrectedSymbolCount(void);
void protocolResetReceiver(void);

#ifdef __cplusplus
}
#endif

#endif
