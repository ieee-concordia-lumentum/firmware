#pragma once
#include <stdint.h>
#include <stdbool.h>

#define CPU_FREQ      (84000000)
#define SYSTICK_FREQ  (1000)

void system_setup(void);
uint64_t system_get_ticks(void);
