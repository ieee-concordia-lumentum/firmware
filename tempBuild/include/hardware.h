#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hardwareInit(void);
bool canTransmitBit(void);

bool transmitBit(char bitValue);

#ifdef __cplusplus
}
#endif
