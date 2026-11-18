#include "coupe.h"
#include "emu/emu.h"
#include "emu/sdcard.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define ROM_FILE "coupe.rom"
#define NVS_NAMESPACE "coupe"

static coupe_config_t config = {.ram_size = COUPE_RAM_SIZE};
static uint8_t *rom;
static const char *rom_source;
static uint8_t memory_choice = 1;

static int get_memory(void)
{
    return memory_choice;
}

static void set_memory(int index)
{
    if (index < 0 || index > 1 || index == memory_choice) {
        return;
    }
    memory_choice = index;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u8(handle, "memory", memory_choice) == ESP_OK) {
            nvs_commit(handle);
        }
        nvs_close(handle);
    }
}

static const char *const memory_names[] = {"256 KB", "512 KB", ""};
static const emu_choice_t choices[] = {
    {.label = "Memory", .names = memory_names, .get = get_memory, .set = set_memory},
};

static void reserve_memory(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t saved;
        if (nvs_get_u8(handle, "memory", &saved) == ESP_OK && saved <= 1) {
            memory_choice = saved;
        }
        nvs_close(handle);
    }
    config.ram = heap_caps_malloc(COUPE_RAM_SIZE, MALLOC_CAP_SPIRAM);
    rom = heap_caps_malloc(COUPE_ROM_SIZE, MALLOC_CAP_SPIRAM);
    config.rom = rom;
    config.pictures = heap_caps_calloc(2 * COUPE_FRAME_PIXELS, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    config.contention = heap_caps_malloc(COUPE_TIMING_BYTES, MALLOC_CAP_SPIRAM);
    config.audio = heap_caps_malloc(COUPE_AUDIO_BUFFER_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
}

static bool plausible_rom(const uint8_t *rom)
{
    /* Ignore erased/default partitions. A working ROM 0 starts with DI. */
    return rom[0] == 0xf3 && (rom[0x4000] != 0xff || rom[0x4001] != 0xff);
}

static bool load_rom(uint8_t *rom)
{
    FILE *file = fopen(SDCARD_MOUNT_POINT "/coupe/roms/" ROM_FILE, "rb");
    if (file) {
        bool ok = fread(rom, COUPE_ROM_SIZE, 1, file) == 1 &&
                  fgetc(file) == EOF && !ferror(file) && plausible_rom(rom);
        fclose(file);
        if (ok) {
            rom_source = "SD card";
            return true;
        }
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "coupe_rom");
    const void *mapped;
    esp_partition_mmap_handle_t handle;
    if (!part || esp_partition_mmap(part, 0, COUPE_ROM_SIZE, ESP_PARTITION_MMAP_DATA,
                                    &mapped, &handle) != ESP_OK) {
        return false;
    }
    bool ok = plausible_rom(mapped);
    if (ok) {
        memcpy(rom, mapped, COUPE_ROM_SIZE);
        rom_source = "flash";
    }
    esp_partition_munmap(handle);
    return ok;
}

static bool start(void)
{
    if (!config.ram || !config.rom || !config.pictures || !config.contention || !config.audio) {
        emu_set_message("Not enough PSRAM for the SAM Coupe.");
        return false;
    }
    if (!load_rom(rom)) {
        emu_set_message("Put the 32 KiB coupe.rom in coupe/roms on the SD card or flash it with the firmware.");
        return false;
    }
    config.ram_size = memory_choice ? COUPE_RAM_SIZE : COUPE_RAM_256K;
    coupe_init(&config);
    return true;
}

static void storage(bool mounted)
{
    emu_set_drive_info(mounted ? "Floppy images in coupe/ on the SD card" : "No SD card");
}

static void screen_mode(emu_screen_t *mode)
{
    *mode = (emu_screen_t){.width = COUPE_WIDTH, .height = COUPE_HEIGHT, .hz = COUPE_HZ};
}

static const char *const drive_names[] = {"Drive 1", "Drive 2"};
static const char *const extensions[] = {".mgt", ".dsk", ".cpm", NULL};
static const disks_size_t disk_sizes[] = {
    {COUPE_DISK_BYTES, "800 KiB (SAMDOS, MasterDOS)"},
    {737280, "720 KiB (Pro-DOS CP/M)"},
};

static const char *extension_for(uint32_t bytes)
{
    return bytes == 737280 ? ".cpm" : ".mgt";
}

static const disks_config_t disks = {
    .folder = "coupe",
    .extensions = extensions,
    .description = "SAM floppy images (.MGT, .DSK raw or EDSK, Pro-DOS .CPM)",
    .title = "Floppy disks",
    .button = "Floppy",
    .nvs_namespace = NVS_NAMESPACE,
    .drives = COUPE_DISK_COUNT,
    .drive_names = drive_names,
    .new_prefix = "SAM",
    .new_extension = ".mgt",
    .sizes = disk_sizes,
    .size_count = sizeof(disk_sizes) / sizeof(disk_sizes[0]),
    .extension_for = extension_for,
    .max_upload = 1024 * 1024,         /* EDSK images run to ~880 KB */
    .insert = coupe_disk_insert,
    .eject = coupe_disk_eject,
    .path = coupe_disk_path,
    .write_protected = coupe_disk_write_protected,
    .create = coupe_disk_create,
};

static void status(emu_json_t *json, int section)
{
    if (section == EMU_STATUS_DRIVES) {
        for (int drive = 0; drive < COUPE_DISK_COUNT; drive++) {
            emu_json_drive(json, drive_names[drive], disks_name(drive),
                           coupe_disk_write_protected(drive), false, 0, 0, coupe_disk_kind(drive));
        }
        return;
    }
    coupe_status_t state;
    coupe_get_status(&state);
    char text[640];
    unsigned pages = (unsigned)(config.ram_size / 16384);
    snprintf(text, sizeof(text), "SAM Coupé: a Z80B at 6 MHz, %u KB RAM (%u pages of 16 KB), the ASIC, an SAA1099",
             (unsigned)(config.ram_size / 1024), pages);
    emu_json_row(json, "System", text);
    snprintf(text, sizeof(text), "32 KB from %s; ROM 0 at &amp;0000 %s, ROM 1 at &amp;C000 %s",
             rom_source ? (strcmp(rom_source, "flash") == 0 ? "the flash" : "the SD card") : "(not loaded yet)",
             (state.lmpr & 0x20) ? "paged out" : "in", (state.lmpr & 0x40) ? "in" : "paged out");
    emu_json_row(json, "ROM", text);

    /* the four 16 KB sections: A and B from LMPR, C and D from HMPR */
    unsigned low = state.lmpr & 31, high = state.hmpr & 31;
    char a_text[24], d_text[24];
    snprintf(a_text, sizeof(a_text), (state.lmpr & 0x20) ? "page %u" : "ROM 0", low);
    snprintf(d_text, sizeof(d_text), (state.lmpr & 0x40) ? "ROM 1" : "page %u", (high + 1) & 31);
    snprintf(text, sizeof(text), "A &amp;0000 %s%s, B &amp;4000 page %u, C &amp;8000 page %u, D &amp;C000 %s%s",
             a_text, (state.lmpr & 0xa0) == 0xa0 ? " (write-protected)" : "", (low + 1) & 31, high, d_text,
             (state.hmpr & 0x80) ? "; C and D on the external memory" : "");
    emu_json_row(json, "Memory", text);

    static const char *const modes[4] = {
        "256 × 192, 2 colours a cell of 8 × 8 (attributes, as a ZX Spectrum)",
        "256 × 192, 2 colours a cell of 8 × 1 (attributes)",
        "512 × 192, 4 colours",
        "256 × 192, 16 colours",
    };
    static const char *const sizes[4] = {"6.75 KB", "12 KB", "24 KB", "24 KB"};
    unsigned mode = (state.vmpr >> 5) & 3, page = state.vmpr & 31;
    unsigned border_colour = (state.border & 7) | ((state.border >> 2) & 8);
    char where[32];
    if (mode >= 2) {
        snprintf(where, sizeof(where), "pages %u-%u", page & 30, (page & 30) + 1);
    } else {
        snprintf(where, sizeof(where), "page %u", page);
    }
    snprintf(text, sizeof(text), "Mode %u: %s; %s in %s; border palette entry %u%s", mode + 1, modes[mode],
             sizes[mode], where, border_colour,
             mode >= 2 && (state.border & 0x80) ? "; the screen off (border bit 7)" : "");
    emu_json_row(json, "Screen", text);

    /* the 16 palette entries as swatches, each one of the 128 colours */
    /* (compact: the whole status reply has 4 KB, esp32emu's STATUS_SIZE) */
    static char swatches[1024];
    size_t n = (size_t)snprintf(swatches, sizeof(swatches), "16 of 128 colours: ");
    for (int i = 0; i < 16 && n < sizeof(swatches); i++) {
        unsigned v = state.clut[i], half = (v >> 3) & 1;
        unsigned r = (((v >> 3) & 4) | (v & 2) | half) * 255 / 7;
        unsigned g = (((v >> 4) & 4) | ((v >> 1) & 2) | half) * 255 / 7;
        unsigned b = (((v >> 2) & 4) | ((v << 1) & 2) | half) * 255 / 7;
        n += (size_t)snprintf(swatches + n, sizeof(swatches) - n,
                              "<b style=\"background:#%02x%02x%02x\">&nbsp;&nbsp;&nbsp;</b> ", r, g, b);
    }
    emu_json_row(json, "Palette", swatches);
    if (state.line_irq < 192) {
        snprintf(text, sizeof(text), "On screen line %u", state.line_irq);
    } else {
        snprintf(text, sizeof(text), "Off (frame interrupt only, 50 Hz)");
    }
    emu_json_row(json, "Line interrupt", text);

    coupe_sound_status_t sound;
    coupe_get_sound(&sound);
    if (!sound.enabled) {
        snprintf(text, sizeof(text), "SAA1099 off%s", sound.beeper ? "; the beeper high" : "");
    } else {
        n = (size_t)snprintf(text, sizeof(text), "SAA1099 on");
        int sounding = 0;
        for (int c = 0; c < 6 && n < sizeof(text); c++) {
            bool tone = sound.tones & (1 << c), noise = sound.noises & (1 << c);
            unsigned left = sound.amplitude[c] & 15, right = sound.amplitude[c] >> 4;
            if ((!tone && !noise) || (left == 0 && right == 0)) {
                continue;
            }
            char what[32];
            if (tone) {
                snprintf(what, sizeof(what), "%u Hz%s", sound.hz[c], noise ? " and noise" : "");
            } else {
                snprintf(what, sizeof(what), "noise");
            }
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s channel %d: %s, volume %u/%u",
                                  sounding++ ? ";" : ":", c + 1, what, left, right);
        }
        if (!sounding) {
            snprintf(text, sizeof(text), "SAA1099 on, silent");
        }
    }
    emu_json_row(json, "Sound", text);
    emu_json_row(json, "Keyboard", "By position; Shift, Ctrl CNTRL, left Alt SYMBOL, right Alt EDIT, Esc ESC, "
                 "` INV, Caps Lock CAPS, Backspace and Delete DELETE, F1-F10 the SAM's F1-F9 and F0, "
                 "the ISO # key :, keypad + +");
    emu_json_row(json, "Disks", "Two 3.5\" drives (a 1772 each): 800 KB MGT, 720 KB Pro-DOS CP/M (.cpm) and EDSK "
                 "images; F9 boots the disk in Drive 1");
}

static void console_info(char *text, size_t size)
{
    coupe_status_t state;
    coupe_get_status(&state);
    snprintf(text, size, "frames %lu PC $%04X mode %u", (unsigned long)state.frames, state.pc, state.mode);
}

/*   k <hex>            tap a key by its HID usage (0x42 F9: boot the disk)
 *   K <hex> <0|1>      a key down or up
 */
static bool console_command(const char *line)
{
    unsigned usage = 0, down = 0;
    if (line[0] == 'k' && sscanf(line + 1, "%x", &usage) == 1) {
        coupe_hid_key(usage, true);
        vTaskDelay(pdMS_TO_TICKS(120));
        coupe_hid_key(usage, false);
        return true;
    }
    if (line[0] == 'K' && sscanf(line + 1, "%x %u", &usage, &down) == 2) {
        coupe_hid_key(usage, down != 0);
        return true;
    }
    return false;
}

static const emu_machine_t machine = {
    /* The board's LVGL font has the ASCII letters used here. */
    .name = "SAM Coupe",
    .short_name = "SAM",
    .view_width = 820,
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = coupe_reset,
    .run_frame = coupe_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = coupe_screen_signature,
    .render_line = coupe_render_line,
    .audio_hz = COUPE_AUDIO_HZ,
    .audio_render = coupe_audio_render,
    .hid_key = coupe_hid_key,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
};

void app_main(void)
{
    emu_run(&machine);
}
