#ifndef HARDWARE_H
#define HARDWARE_H

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

#endif // HARDWARE_H

