#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hardwareInit(void);
bool canTransmitBit(void);

#ifdef __cplusplus
}
#endif
