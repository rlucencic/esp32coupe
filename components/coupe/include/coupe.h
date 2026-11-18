#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    COUPE_ROM_SIZE = 32 * 1024,
    COUPE_RAM_256K = 256 * 1024,
    COUPE_RAM_SIZE = 512 * 1024,
    COUPE_WIDTH = 576,
    COUPE_HEIGHT = 240,
    COUPE_HZ = 50,
    COUPE_AUDIO_HZ = 48000,
    COUPE_AUDIO_BUFFER_FRAMES = 4096,
    COUPE_FRAME_PIXELS = COUPE_WIDTH * COUPE_HEIGHT,
    COUPE_TIMING_BYTES = 312 * 384 + 64,
    COUPE_DISK_BYTES = 819200,
    COUPE_DISK_COUNT = 2,
};

typedef struct {
    uint8_t *ram;
    size_t ram_size;
    const uint8_t *rom;
    uint16_t *pictures;
    uint8_t *contention;
    int16_t *audio;
} coupe_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    uint8_t mode;
} coupe_status_t;

void coupe_init(const coupe_config_t *config);
void coupe_reset(void);
void coupe_run_frame(void);
void coupe_render_line(int y, uint16_t *out);
uint32_t coupe_screen_signature(void);
void coupe_get_status(coupe_status_t *status);
int coupe_audio_render(int16_t *out, int max_frames);
void coupe_hid_key(uint8_t usage, bool pressed);

bool coupe_disk_insert(int drive, const char *path);
void coupe_disk_eject(int drive);
const char *coupe_disk_path(int drive);
bool coupe_disk_write_protected(int drive);
bool coupe_disk_create(const char *path, uint32_t bytes);
