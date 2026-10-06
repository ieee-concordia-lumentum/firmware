#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hardwareInit(void);
#ifdef SENDER
typedef enum { Low = 0, High = 1 } GpioState;
void transmitBit(GpioState bit);
#endif

#ifdef __cplusplus
}
#endif

#endif
