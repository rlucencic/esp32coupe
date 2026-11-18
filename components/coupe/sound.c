/* SAA1099 stereo tone/noise mixer and the ASIC beeper. */
#include "coupe.h"
#include "coupe_lock.h"
#include "sound.h"

#include <string.h>

enum {
    FRAME_SAMPLES = COUPE_AUDIO_HZ / COUPE_HZ,
    FRAME_CYCLES = 312 * 384,
};

static int16_t *ring;
static unsigned read_at, queued;
static coupe_lock_t guard = COUPE_LOCK_INIT;
static int16_t frame[FRAME_SAMPLES * 2];
static int cursor;
static bool beep;
static uint8_t address, registers[32];
static unsigned tone_phase[6], tone_step[6];
static unsigned noise_phase[2], noise_state[2];

static unsigned phase_step(unsigned numerator, unsigned divider)
{
    return ((uint64_t)numerator << 16) / (divider * COUPE_AUDIO_HZ);
}

static void update_pitch(void)
{
    for (unsigned i = 0; i < 6; i++) {
        unsigned octave = (registers[16 + i / 2] >> (4 * (i % 2))) & 7;
        tone_step[i] = phase_step(15625u << octave, 511 - registers[8 + i]);
    }
}

void coupe_sound_reset(int16_t *samples)
{
    cursor = 0;
    beep = false;
    address = 0;
    memset(registers, 0, sizeof(registers));
    memset(tone_phase, 0, sizeof(tone_phase));
    memset(noise_phase, 0, sizeof(noise_phase));
    noise_state[0] = noise_state[1] = 1;
    update_pitch();
    coupe_lock(&guard);
    ring = samples;
    read_at = queued = 0;
    coupe_unlock(&guard);
}

void coupe_get_sound(coupe_sound_status_t *status)
{
    status->enabled = registers[28] & 1;
    for (unsigned i = 0; i < 6; i++) {
        unsigned octave = (registers[16 + i / 2] >> (4 * (i % 2))) & 7;
        status->amplitude[i] = registers[i];
        status->hz[i] = (15625u << octave) / (511 - registers[8 + i]);
    }
    status->tones = registers[20] & 63;
    status->noises = registers[21] & 63;
    status->beeper = beep;
}

void coupe_sound_beeper(bool level)
{
    beep = level;
}

void coupe_sound_select(uint8_t value)
{
    address = value & 31;
}

void coupe_sound_write(uint8_t value)
{
    registers[address] = value;
    if ((address >= 8 && address <= 13) || (address >= 16 && address <= 18)) {
        update_pitch();
    } else if (address == 28 && (value & 2)) {
        memset(tone_phase, 0, sizeof(tone_phase));
        memset(noise_phase, 0, sizeof(noise_phase));
    }
}

static void sample(int16_t *out)
{
    int left = beep ? 1800 : 0;
    int right = left;
    if ((registers[28] & 3) == 1) {
        static const unsigned noise_frequency[] = {31250, 15625, 7812};
        for (unsigned i = 0; i < 2; i++) {
            unsigned selector = (registers[22] >> (4 * i)) & 3;
            unsigned step = selector == 3 ? tone_step[i * 3] : noise_frequency[selector] * 65536u / COUPE_AUDIO_HZ;
            noise_phase[i] += step;
            if (noise_phase[i] >= 65536) {
                noise_phase[i] -= 65536;
                unsigned feedback = (noise_state[i] ^ (noise_state[i] >> 3)) & 1;
                noise_state[i] = (noise_state[i] >> 1) | (feedback << 17);
            }
        }
        for (unsigned i = 0; i < 6; i++) {
            unsigned mask = 1u << i;
            bool tone = (registers[20] & mask) != 0;
            bool noise = (registers[21] & mask) != 0;
            tone_phase[i] = (tone_phase[i] + tone_step[i]) & 0xffff;
            if (tone || noise) {
                bool high = (!tone || (tone_phase[i] & 0x8000)) &&
                            (!noise || (noise_state[i / 3] & 1));
                int level = high ? 210 : -210;
                left += level * (registers[i] & 15);
                right += level * (registers[i] >> 4);
            }
        }
    }
    out[0] = left;
    out[1] = right;
}

void coupe_sound_advance(int cycle)
{
    if (!ring) {
        return;
    }
    unsigned until = cycle <= 0 ? 0 : cycle >= FRAME_CYCLES ? FRAME_SAMPLES :
                     ((unsigned)cycle * FRAME_SAMPLES + FRAME_CYCLES - 1) / FRAME_CYCLES;
    while ((unsigned)cursor < until) {
        sample(&frame[2 * cursor]);
        cursor++;
    }
}

void coupe_sound_finish_frame(void)
{
    if (!ring) {
        return;
    }
    coupe_sound_advance(FRAME_CYCLES);
    coupe_lock(&guard);
    unsigned free_frames = COUPE_AUDIO_BUFFER_FRAMES - queued;
    if (free_frames < FRAME_SAMPLES) {
        unsigned drop = FRAME_SAMPLES - free_frames;
        read_at = (read_at + drop) % COUPE_AUDIO_BUFFER_FRAMES;
        queued -= drop;
    }
    unsigned write_at = (read_at + queued) % COUPE_AUDIO_BUFFER_FRAMES;
    unsigned first = COUPE_AUDIO_BUFFER_FRAMES - write_at;
    if (first > FRAME_SAMPLES) {
        first = FRAME_SAMPLES;
    }
    memcpy(ring + 2 * write_at, frame, first * 2 * sizeof(*frame));
    memcpy(ring, frame + 2 * first, (FRAME_SAMPLES - first) * 2 * sizeof(*frame));
    queued += FRAME_SAMPLES;
    coupe_unlock(&guard);
    cursor = 0;
}

int coupe_audio_render(int16_t *out, int max_frames)
{
    if (max_frames <= 0) {
        return 0;
    }
    coupe_lock(&guard);
    unsigned count = queued < (unsigned)max_frames ? queued : (unsigned)max_frames;
    if (count) {
        unsigned first = COUPE_AUDIO_BUFFER_FRAMES - read_at;
        if (first > count) {
            first = count;
        }
        memcpy(out, ring + 2 * read_at, first * 2 * sizeof(*out));
        memcpy(out + 2 * first, ring, (count - first) * 2 * sizeof(*out));
        read_at = (read_at + count) % COUPE_AUDIO_BUFFER_FRAMES;
        queued -= count;
    }
    coupe_unlock(&guard);
    return count;
}
