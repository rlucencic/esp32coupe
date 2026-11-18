/* Two independent 1772 register sets on the SAM's floppy ports. */
#include "coupe.h"
#include "floppy.h"

#include <stdio.h>
#include <string.h>

enum {
    TRACKS = 80,
    SECTORS = 10,
    SECTOR_SIZE = 512,
    BUSY = 0x01,
    DRQ = 0x02,
    LOST_DATA = 0x04,
    NOT_FOUND = 0x10,
    WRITE_ERROR = 0x20,
    SPIN_UP = 0x20,
    PROTECTED = 0x40,
    MOTOR_ON = 0x80,
    COMMAND_DELAY = 512,
    FIRST_BYTE_TIMEOUT = 8192,
    DATA_TIMEOUT = 2048,
    REVOLUTION = 1200000,
};

typedef enum {IDLE, SEEK, READ_SECTOR, WRITE_SECTOR, READ_ADDRESS} operation_t;

typedef struct {
    FILE *file;
    bool read_only;
    bool type_one;
    bool multiple;
    uint8_t track, sector, data, cylinder, side, error;
    int step;
    operation_t op;
    uint32_t ready_at, data_deadline;
    unsigned offset, size;
    uint8_t buffer[SECTOR_SIZE];
} disk_t;

static disk_t disks[COUPE_DISK_COUNT];
static char disk_paths[COUPE_DISK_COUNT][512];

static void reset_controller(disk_t *disk)
{
    FILE *file = disk->file;
    bool read_only = disk->read_only;
    *disk = (disk_t){.file = file, .read_only = read_only, .type_one = true, .step = 1};
}

void coupe_fdc_reset(void)
{
    for (int i = 0; i < COUPE_DISK_COUNT; i++) {
        reset_controller(&disks[i]);
    }
}

bool coupe_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= COUPE_DISK_COUNT || !path || strlen(path) >= sizeof(disk_paths[0])) {
        return false;
    }
    FILE *file = fopen(path, "r+b");
    bool read_only = file == NULL;
    if (read_only) {
        file = fopen(path, "rb");
    }
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || ftell(file) != COUPE_DISK_BYTES) {
        fclose(file);
        return false;
    }
    coupe_disk_eject(drive);
    strcpy(disk_paths[drive], path);
    disks[drive].file = file;
    disks[drive].read_only = read_only;
    reset_controller(&disks[drive]);
    return true;
}

void coupe_disk_eject(int drive)
{
    if (drive < 0 || drive >= COUPE_DISK_COUNT) {
        return;
    }
    disk_t *disk = &disks[drive];
    if (disk->file) {
        fclose(disk->file);
    }
    disk->file = NULL;
    disk->read_only = false;
    disk_paths[drive][0] = '\0';
    reset_controller(disk);
}

const char *coupe_disk_path(int drive)
{
    return drive >= 0 && drive < COUPE_DISK_COUNT && disk_paths[drive][0] ? disk_paths[drive] : NULL;
}

bool coupe_disk_write_protected(int drive)
{
    return drive >= 0 && drive < COUPE_DISK_COUNT && disks[drive].read_only;
}

bool coupe_disk_create(const char *path, uint32_t bytes)
{
    if (bytes != COUPE_DISK_BYTES) {
        return false;
    }
    FILE *file = fopen(path, "wbx");
    if (!file) {
        return false;
    }
    static const uint8_t empty[SECTOR_SIZE];
    bool ok = true;
    for (unsigned i = 0; ok && i < COUPE_DISK_BYTES / SECTOR_SIZE; i++) {
        ok = fwrite(empty, SECTOR_SIZE, 1, file) == 1;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove(path);
    }
    return ok;
}

static void finish(disk_t *disk, uint8_t error)
{
    disk->error = error;
    disk->op = IDLE;
}

static bool seek_sector(disk_t *disk)
{
    if (!disk->file || disk->track != disk->cylinder || disk->cylinder >= TRACKS ||
        disk->sector < 1 || disk->sector > SECTORS) {
        return false;
    }
    long sector = (disk->cylinder * 2 + disk->side) * SECTORS + disk->sector - 1;
    return fseek(disk->file, sector * SECTOR_SIZE, SEEK_SET) == 0;
}

static void locate_sector(disk_t *disk, uint32_t cycles)
{
    disk->offset = 0;
    disk->size = SECTOR_SIZE;
    disk->ready_at = cycles + COMMAND_DELAY;
    /* The ROM has a software delay before it reads the first data byte. */
    disk->data_deadline = disk->ready_at + FIRST_BYTE_TIMEOUT;
    if (!seek_sector(disk)) {
        finish(disk, NOT_FOUND);
    } else if (disk->op == READ_SECTOR && fread(disk->buffer, SECTOR_SIZE, 1, disk->file) != 1) {
        finish(disk, NOT_FOUND);
    }
}

static void next_sector(disk_t *disk, uint32_t cycles)
{
    if (disk->multiple && disk->sector < SECTORS) {
        disk->sector++;
        locate_sector(disk, cycles);
    } else {
        finish(disk, 0);
    }
}

static void seek_track(disk_t *disk, uint8_t command, uint32_t cycles)
{
    unsigned type = command >> 4;
    int cylinder = disk->cylinder;
    if (type == 0 || type == 1) {
        int target = type == 0 ? 0 : disk->data;
        disk->step = target >= cylinder ? 1 : -1;
        cylinder = target;
    } else {
        if (type >= 4) {
            disk->step = type < 6 ? 1 : -1;
        }
        cylinder += disk->step;
    }
    disk->cylinder = cylinder < 0 ? 0 : cylinder >= TRACKS ? TRACKS - 1 : cylinder;
    if (type == 1) {
        disk->track = disk->data;
    } else if (type == 0 || (type & 1)) {
        disk->track = disk->cylinder;
    }
    disk->op = SEEK;
    disk->ready_at = cycles + COMMAND_DELAY;
}

static void read_address(disk_t *disk, uint32_t cycles)
{
    if (!disk->file) {
        finish(disk, NOT_FOUND);
        return;
    }
    uint8_t sector = 1 + (cycles % REVOLUTION) / (REVOLUTION / SECTORS);
    uint8_t address[] = {0xa1, 0xa1, 0xa1, 0xfe, disk->cylinder, disk->side, sector, 2};
    unsigned crc = 0xffff;
    for (unsigned i = 0; i < sizeof(address); i++) {
        crc ^= (unsigned)address[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
        }
    }
    memcpy(disk->buffer, address + 4, 4);
    disk->buffer[4] = crc >> 8;
    disk->buffer[5] = crc;
    disk->offset = 0;
    disk->size = 6;
    disk->ready_at = cycles + COMMAND_DELAY;
    disk->data_deadline = disk->ready_at + FIRST_BYTE_TIMEOUT;
    disk->op = READ_ADDRESS;
    disk->sector = disk->cylinder;
}

static bool ready(const disk_t *disk, uint32_t cycles)
{
    return (int32_t)(cycles - disk->ready_at) >= 0;
}

static void update_status(disk_t *disk, uint32_t cycles)
{
    if (disk->op == SEEK) {
        if (ready(disk, cycles)) {
            finish(disk, 0);
        }
    } else if (disk->op != IDLE && (int32_t)(cycles - disk->data_deadline) >= 0) {
        /* The boot ROM can leave the tail of its last sector unread. */
        finish(disk, LOST_DATA);
    }
}

uint8_t coupe_fdc_read(int drive, int port, uint32_t cycles)
{
    disk_t *disk = &disks[drive];
    update_status(disk, cycles);
    int reg = port & 3;
    if (reg == 0) {
        uint8_t status = disk->error | (disk->file ? MOTOR_ON : 0);
        if (disk->read_only) {
            status |= PROTECTED;
        }
        if (disk->op != IDLE) {
            status |= BUSY;
            if (disk->op != SEEK && ready(disk, cycles)) {
                status |= DRQ;
            }
        }
        if (disk->type_one) {
            if (disk->cylinder == 0) {
                status |= 4;
            }
            if (disk->file && cycles % REVOLUTION < 15000) {
                status |= DRQ; /* index in type I status */
            }
            if (disk->file) {
                status |= SPIN_UP;
            }
        }
        return status;
    }
    if (reg == 1) {
        return disk->track;
    }
    if (reg == 2) {
        return disk->sector;
    }
    if ((disk->op == READ_SECTOR || disk->op == READ_ADDRESS) && ready(disk, cycles)) {
        disk->data_deadline = cycles + DATA_TIMEOUT;
        disk->data = disk->buffer[disk->offset++];
        if (disk->offset == disk->size) {
            if (disk->op == READ_ADDRESS) {
                finish(disk, 0);
            } else {
                next_sector(disk, cycles);
            }
        }
    }
    return disk->data;
}

void coupe_fdc_write(int drive, int port, uint8_t value, uint32_t cycles)
{
    disk_t *disk = &disks[drive];
    update_status(disk, cycles);
    switch (port & 3) {
    case 0:
        if ((value & 0xf0) == 0xd0) {
            finish(disk, 0);
            disk->type_one = true;
            return;
        }
        if (disk->op != IDLE) {
            return;
        }
        disk->side = (port >> 2) & 1;
        disk->error = 0;
        disk->type_one = !(value & 0x80);
        disk->multiple = (value & 0x10) != 0;
        if (disk->type_one) {
            seek_track(disk, value, cycles);
        } else if (value < 0xc0) {
            disk->op = value < 0xa0 ? READ_SECTOR : WRITE_SECTOR;
            if (disk->op == WRITE_SECTOR && disk->read_only) {
                finish(disk, PROTECTED);
            } else {
                locate_sector(disk, cycles);
            }
        } else if ((value & 0xf0) == 0xc0) {
            read_address(disk, cycles);
        } else {
            finish(disk, NOT_FOUND); /* raw track commands are not implemented */
        }
        break;
    case 1:
        if (disk->op == IDLE) {
            disk->track = value;
        }
        break;
    case 2:
        if (disk->op == IDLE) {
            disk->sector = value;
        }
        break;
    case 3:
        disk->data = value;
        if (disk->op == WRITE_SECTOR && ready(disk, cycles)) {
            disk->data_deadline = cycles + DATA_TIMEOUT;
            disk->buffer[disk->offset++] = value;
            if (disk->offset == disk->size) {
                if (fwrite(disk->buffer, SECTOR_SIZE, 1, disk->file) != 1 || fflush(disk->file) != 0) {
                    finish(disk, WRITE_ERROR);
                } else {
                    next_sector(disk, cycles);
                }
            }
        }
        break;
    }
}
