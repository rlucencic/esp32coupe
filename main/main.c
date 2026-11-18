#include "coupe.h"
#include "emu/emu.h"
#include "emu/sdcard.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
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
static const char *const extensions[] = {".mgt", ".dsk", NULL};
static const disks_size_t disk_sizes[] = {{COUPE_DISK_BYTES, "800 KiB"}};

static const disks_config_t disks = {
    .folder = "coupe",
    .extensions = extensions,
    .description = "SAM floppy images (.MGT, raw .DSK)",
    .title = "Floppy disks",
    .button = "Floppy",
    .nvs_namespace = NVS_NAMESPACE,
    .drives = COUPE_DISK_COUNT,
    .drive_names = drive_names,
    .new_prefix = "SAM",
    .new_extension = ".mgt",
    .sizes = disk_sizes,
    .size_count = 1,
    .max_upload = COUPE_DISK_BYTES,
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
                           coupe_disk_write_protected(drive), false, 0, 0, NULL);
        }
        return;
    }
    coupe_status_t state;
    coupe_get_status(&state);
    char text[40];
    snprintf(text, sizeof(text), "SAM Coupe, Z80B, %u KiB", (unsigned)(config.ram_size / 1024));
    emu_json_row(json, "System", text);
    emu_json_row(json, "ROM", rom_source ? rom_source : "Waiting for startup");
    snprintf(text, sizeof(text), "Mode %u", state.mode);
    emu_json_row(json, "Screen", text);
}

static void console_info(char *text, size_t size)
{
    coupe_status_t state;
    coupe_get_status(&state);
    snprintf(text, size, "frames %lu PC $%04X mode %u", (unsigned long)state.frames, state.pc, state.mode);
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
};

void app_main(void)
{
    emu_run(&machine);
}
