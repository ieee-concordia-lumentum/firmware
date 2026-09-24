#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void protocolInit(void);
void protocolTimerTick(void);
void protocolTransmit(const char *message);

#ifdef __cplusplus
}
#endif
