#pragma once

#include <stdbool.h>
#include <stdint.h>

void coupe_sound_reset(int16_t *samples);
void coupe_sound_advance(int cycle);
void coupe_sound_finish_frame(void);
void coupe_sound_beeper(bool level);
void coupe_sound_select(uint8_t address);
void coupe_sound_write(uint8_t value);
