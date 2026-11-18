/* Boot a local ROM with the board-independent machine core.
 *
 * COUPE_KEYS types (\n Enter, \b F9, \w five seconds, \d drive 1 changed
 * to the image in COUPE_SWAP); COUPE_DISK2 mounts an image in drive 2.
 */
#include "coupe.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[COUPE_RAM_SIZE], rom[COUPE_ROM_SIZE], timing[COUPE_TIMING_BYTES];
static uint16_t pictures[2 * COUPE_FRAME_PIXELS];

static void run_frames(long count)
{
    for (long frame = 0; frame < count; frame++) {
        coupe_run_frame();
    }
}

static bool type_text(const char *text)
{
    for (; text && *text; text++) {
        uint8_t usage;
        bool shift = false;
        char c = *text;
        if (c >= 'A' && c <= 'Z') {
            usage = 4 + c - 'A';
        } else if (c >= '0' && c <= '9') {
            usage = c == '0' ? 0x27 : 0x1e + c - '1';
        } else if (c == '\\' && text[1] == 'n') {
            usage = 0x28;
            text++;
        } else if (c == '\\' && text[1] == 'b') {
            usage = 0x42;  /* SAM F9: boot SAMDOS */
            text++;
        } else if (c == '\\' && text[1] == 'w') {
            text++;
            run_frames(250);
            continue;
        } else if (c == '\\' && text[1] == 'd') {
            text++;
            const char *swap = getenv("COUPE_SWAP");
            if (!swap || !coupe_disk_insert(0, swap)) {
                fprintf(stderr, "cannot mount COUPE_SWAP\n");
                return false;
            }
            continue;
        } else {
            switch (c) {
            case ' ': usage = 0x2c; break;
            case '+': usage = 0x57; break;
            case '"': usage = 0x34; break;
            case '.': usage = 0x37; break;
            case ':': usage = 0x32; break;     /* the SAM's own : key (ISO #) */
            default: return false;
            }
        }
        if (shift) {
            coupe_hid_key(0xe1, true);
            run_frames(2);
        }
        coupe_hid_key(usage, true);
        run_frames(6);
        coupe_hid_key(usage, false);
        if (shift) {
            coupe_hid_key(0xe1, false);
        }
        run_frames(6);
        if (usage == 0x42) {
            run_frames(180);
        }
    }
    return true;
}

static bool screenshot(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    fprintf(file, "P6\n%d %d\n255\n", COUPE_WIDTH, COUPE_HEIGHT);
    for (int y = 0; y < COUPE_HEIGHT; y++) {
        uint16_t line[COUPE_WIDTH];
        uint8_t rgb[COUPE_WIDTH * 3];
        coupe_render_line(y, line);
        for (int x = 0; x < COUPE_WIDTH; x++) {
            rgb[x * 3] = ((line[x] >> 11) & 31) * 255 / 31;
            rgb[x * 3 + 1] = ((line[x] >> 5) & 63) * 255 / 63;
            rgb[x * 3 + 2] = (line[x] & 31) * 255 / 31;
        }
        if (fwrite(rgb, sizeof(rgb), 1, file) != 1) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s rom frames screen.ppm [drive1.mgt]\n", argv[0]);
        return 2;
    }
    char *end;
    errno = 0;
    long count = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || count <= 0 || count > INT_MAX) {
        fprintf(stderr, "invalid frame count: %s\n", argv[2]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 1;
    }
    bool ok = fread(rom, sizeof(rom), 1, file) == 1 && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) {
        fprintf(stderr, "expected a 32 KiB ROM\n");
        return 1;
    }
    size_t ram_size = COUPE_RAM_SIZE;
    const char *memory = getenv("COUPE_MEMORY");
    if (memory) {
        if (strcmp(memory, "256") == 0) {
            ram_size = COUPE_RAM_256K;
        } else if (strcmp(memory, "512") != 0) {
            fprintf(stderr, "COUPE_MEMORY must be 256 or 512\n");
            return 2;
        }
    }
    const coupe_config_t config = {
        .ram = ram, .ram_size = ram_size, .rom = rom, .pictures = pictures, .contention = timing,
    };
    coupe_init(&config);
    if (argc == 5 && !coupe_disk_insert(0, argv[4])) {
        fprintf(stderr, "cannot mount %s\n", argv[4]);
        return 1;
    }
    const char *second = getenv("COUPE_DISK2");
    if (second && !coupe_disk_insert(1, second)) {
        fprintf(stderr, "cannot mount %s\n", second);
        return 1;
    }
    run_frames(count);
    const char *keys = getenv("COUPE_KEYS");
    if (!type_text(keys)) {
        fprintf(stderr, "unsupported character in COUPE_KEYS\n");
        return 2;
    }
    if (keys) {
        run_frames(50);
    }
    coupe_status_t status;
    coupe_get_status(&status);
    printf("%lu frames, PC $%04X, mode %u\n", (unsigned long)status.frames, status.pc, status.mode);
    coupe_disk_eject(0);
    return !screenshot(argv[3]);
}
