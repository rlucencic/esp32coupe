#pragma once

#include <stdint.h>

void coupe_fdc_reset(void);
uint8_t coupe_fdc_read(int drive, int port, uint32_t cycles);
void coupe_fdc_write(int drive, int port, uint8_t value, uint32_t cycles);
