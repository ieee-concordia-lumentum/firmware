#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hardwareInit(void);
bool transmitBit(char bitValue);

#ifdef __cplusplus
}
#endif

#endif
