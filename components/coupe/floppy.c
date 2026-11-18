/* Two independent 1772 register sets on the SAM's floppy ports.
 *
 * A mounted image is a table of sectors per track (cylinder, side): each
 * sector's ID (C, H, R, N), its size and where its data is in the file.
 * The controller finds a sector by its ID, as the 1772 does: C against the
 * track register, R against the sector register (no side compare).
 *
 *   raw 819,200 bytes   MGT: 80 cylinders, 2 sides, 10 sectors of 512 (IDs
 *                       1-10), both sides of a cylinder consecutive (.mgt,
 *                       .dsk)
 *   raw 737,280 bytes   Pro-DOS's CP/M format: the same with 9 sectors
 *                       (IDs 1-9) (.cpm, .dsk)
 *   EDSK                "EXTENDED CPC DSK File": every track's own sectors,
 *                       any IDs and sizes (copy-protected boot disks: three
 *                       1 KB sectors numbered 253-255), CRC errors and
 *                       deleted data from the image's status bytes (.dsk)
 *
 * Writes go to the image in place, a sector at a time. Formatting (Write
 * Track) is not implemented.
 */
#include "coupe.h"
#include "floppy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CYLINDERS = 84,                     /* what a drive can step to (images: 80-83) */
    CHUNK = 1024,                       /* larger sectors stream through the buffer */
    BUSY = 0x01,
    DRQ = 0x02,
    LOST_DATA = 0x04,
    CRC_ERROR = 0x08,
    NOT_FOUND = 0x10,
    RECORD_TYPE = 0x20,                 /* type II read: a deleted data mark */
    WRITE_ERROR = 0x20,
    SPIN_UP = 0x20,
    PROTECTED = 0x40,
    MOTOR_ON = 0x80,
    COMMAND_DELAY = 512,
    FIRST_BYTE_TIMEOUT = 8192,
    DATA_TIMEOUT = 2048,
    REVOLUTION = 1200000,
    RAW_MGT_BYTES = 819200,
    RAW_PRODOS_BYTES = 737280,
};

typedef enum {IDLE, SEEK, READ_SECTOR, WRITE_SECTOR, READ_ADDRESS} operation_t;

typedef struct {
    uint8_t c, h, r, n;
    uint8_t st1, st2;                   /* EDSK: the 765's status (CRC error, deleted data) */
    uint16_t size;                      /* the data's bytes */
    uint32_t offset;                    /* in the file */
} sector_t;

typedef struct {
    uint16_t first;                     /* into the drive's sector table */
    uint8_t count;
} track_t;

typedef struct {
    sector_t *sectors;                  /* malloc'd at insert */
    track_t tracks[CYLINDERS][2];
} layout_t;

typedef struct {
    FILE *file;
    bool read_only;
    layout_t *layout;
    bool type_one;
    bool multiple;
    uint8_t track, sector, data, cylinder, side, error;
    int step;
    operation_t op;
    uint32_t ready_at, data_deadline;
    const sector_t *current;            /* the sector being read or written */
    unsigned offset, size;              /* into it */
    uint8_t buffer[CHUNK];
} disk_t;

static disk_t disks[COUPE_DISK_COUNT];
static char disk_paths[COUPE_DISK_COUNT][512];
static const char *disk_kinds[COUPE_DISK_COUNT];

static void reset_controller(disk_t *disk)
{
    FILE *file = disk->file;
    bool read_only = disk->read_only;
    layout_t *layout = disk->layout;
    *disk = (disk_t){.file = file, .read_only = read_only, .layout = layout, .type_one = true, .step = 1};
}

void coupe_fdc_reset(void)
{
    for (int i = 0; i < COUPE_DISK_COUNT; i++) {
        reset_controller(&disks[i]);
    }
}

/* ---- Images to sector tables ---- */

static void free_layout(layout_t *layout)
{
    if (layout) {
        free(layout->sectors);
        free(layout);
    }
}

/* A raw image: cylinders of two sides, each `spt` sectors of 512 numbered 1-spt */
static layout_t *raw_layout(int spt)
{
    layout_t *layout = calloc(1, sizeof(*layout));
    sector_t *s = layout ? malloc(sizeof(sector_t) * 80 * 2 * spt) : NULL;
    if (!s) {
        free(layout);
        return NULL;
    }
    layout->sectors = s;
    unsigned n = 0;
    for (int c = 0; c < 80; c++) {
        for (int h = 0; h < 2; h++) {
            layout->tracks[c][h] = (track_t){.first = n, .count = spt};
            for (int r = 1; r <= spt; r++, n++) {
                s[n] = (sector_t){.c = c, .h = h, .r = r, .n = 2, .size = 512,
                                  .offset = (uint32_t)n * 512};
            }
        }
    }
    return layout;
}

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* An EDSK image: a 256-byte disk header (cylinders at $30, sides at $31, each
 * track's size / 256 from $34), then each track: a 256-byte "Track-Info"
 * header (cylinder $10, side $11, sectors $15, from $18 eight bytes a sector:
 * C H R N ST1 ST2 and its data length) and its sectors' data
 */
static layout_t *edsk_layout(FILE *file, long length)
{
    uint8_t header[256];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(header, sizeof(header), 1, file) != 1) {
        return NULL;
    }
    int cylinders = header[0x30], sides = header[0x31];
    if (cylinders == 0 || cylinders > CYLINDERS || sides < 1 || sides > 2) {
        return NULL;
    }
    unsigned total = 0;
    uint32_t offset = 256;
    uint8_t info[256];
    for (int t = 0; t < cylinders * sides; t++) {
        uint32_t size = header[0x34 + t] * 256u;
        if (size == 0) {
            continue;                   /* an unformatted track */
        }
        if (offset + size > (uint32_t)length || fseek(file, offset, SEEK_SET) != 0 ||
            fread(info, sizeof(info), 1, file) != 1 || memcmp(info, "Track-Info", 10) != 0) {
            return NULL;
        }
        total += info[0x15];
        offset += size;
    }
    layout_t *layout = calloc(1, sizeof(*layout));
    sector_t *s = layout ? malloc(sizeof(sector_t) * (total ? total : 1)) : NULL;
    if (!s) {
        free(layout);
        return NULL;
    }
    layout->sectors = s;
    unsigned n = 0;
    offset = 256;
    for (int t = 0; t < cylinders * sides; t++) {
        uint32_t size = header[0x34 + t] * 256u;
        if (size == 0) {
            continue;
        }
        if (fseek(file, offset, SEEK_SET) != 0 || fread(info, sizeof(info), 1, file) != 1) {
            free_layout(layout);
            return NULL;
        }
        int c = t / sides, h = t % sides, count = info[0x15];
        if (count > (256 - 0x18) / 8) {
            free_layout(layout);
            return NULL;
        }
        layout->tracks[c][h] = (track_t){.first = n, .count = count};
        uint32_t data = offset + 256;
        for (int k = 0; k < count; k++, n++) {
            const uint8_t *id = info + 0x18 + 8 * k;
            uint32_t length_in_image = le16(id + 6);
            uint32_t natural = 128u << (id[3] & 7);
            /* (more than N says: copies of a weak sector; the first one) */
            uint32_t bytes = length_in_image < natural ? length_in_image : natural;
            s[n] = (sector_t){.c = id[0], .h = id[1], .r = id[2], .n = id[3], .st1 = id[4], .st2 = id[5],
                              .size = bytes, .offset = data};
            data += length_in_image;
        }
        offset += size;
    }
    return layout;
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
    char magic[16] = {0};
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    rewind(file);
    size_t got = fread(magic, 1, sizeof(magic), file);
    layout_t *layout = NULL;
    const char *kind = NULL;
    if (got == sizeof(magic) && memcmp(magic, "EXTENDED CPC DSK", 16) == 0) {
        layout = edsk_layout(file, length);
        kind = "EDSK";
    } else if (length == RAW_MGT_BYTES) {
        layout = raw_layout(10);
        kind = "MGT";
    } else if (length == RAW_PRODOS_BYTES) {
        layout = raw_layout(9);
        kind = "Pro-DOS CP/M";
    }
    if (!layout) {
        fclose(file);
        return false;
    }
    coupe_disk_eject(drive);
    strcpy(disk_paths[drive], path);
    disks[drive].file = file;
    disks[drive].read_only = read_only;
    disks[drive].layout = layout;
    disk_kinds[drive] = kind;
    reset_controller(&disks[drive]);
    return true;
}

const char *coupe_disk_kind(int drive)
{
    return drive >= 0 && drive < COUPE_DISK_COUNT && disks[drive].file ? disk_kinds[drive] : NULL;
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
    free_layout(disk->layout);
    disk->file = NULL;
    disk->layout = NULL;
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
    static const uint8_t empty[512];
    bool ok = true;
    for (unsigned i = 0; ok && i < COUPE_DISK_BYTES / sizeof(empty); i++) {
        ok = fwrite(empty, sizeof(empty), 1, file) == 1;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove(path);
    }
    return ok;
}

/* ---- The controller ---- */

static void finish(disk_t *disk, uint8_t error)
{
    disk->error = error;
    disk->op = IDLE;
}

static const track_t *head_track(const disk_t *disk)
{
    if (!disk->file || !disk->layout || disk->cylinder >= CYLINDERS) {
        return NULL;
    }
    const track_t *t = &disk->layout->tracks[disk->cylinder][disk->side];
    return t->count ? t : NULL;
}

/* The sector under the head with C = the track register, R = the sector
 * register
 */
static const sector_t *find_sector(const disk_t *disk)
{
    const track_t *t = head_track(disk);
    for (unsigned i = 0; t && i < t->count; i++) {
        const sector_t *s = &disk->layout->sectors[t->first + i];
        if (s->c == disk->track && s->r == disk->sector) {
            return s;
        }
    }
    return NULL;
}

/* The next chunk of the current sector into the buffer */
static bool fill(disk_t *disk)
{
    unsigned left = disk->size - disk->offset;
    unsigned n = left < CHUNK ? left : CHUNK;
    return fseek(disk->file, disk->current->offset + disk->offset, SEEK_SET) == 0 &&
           fread(disk->buffer, 1, n, disk->file) == n;
}

/* The buffered bytes of the current sector (from the chunk's start) to the file */
static bool flush(disk_t *disk)
{
    unsigned start = (disk->offset - 1) / CHUNK * CHUNK, n = disk->offset - start;
    return fseek(disk->file, disk->current->offset + start, SEEK_SET) == 0 &&
           fwrite(disk->buffer, 1, n, disk->file) == n && fflush(disk->file) == 0;
}

static void locate_sector(disk_t *disk, uint32_t cycles)
{
    disk->offset = 0;
    disk->ready_at = cycles + COMMAND_DELAY;
    /* The ROM has a software delay before it reads the first data byte. */
    disk->data_deadline = disk->ready_at + FIRST_BYTE_TIMEOUT;
    disk->current = find_sector(disk);
    if (!disk->current || disk->current->size == 0) {
        finish(disk, NOT_FOUND);
        return;
    }
    disk->size = disk->current->size;
    if (disk->op == READ_SECTOR && !fill(disk)) {
        finish(disk, NOT_FOUND);
    }
}

/* A sector done: its errors, then the next one for a multiple command */
static void next_sector(disk_t *disk, uint32_t cycles)
{
    const sector_t *s = disk->current;
    uint8_t error = 0;
    if (disk->op == READ_SECTOR) {
        error = ((s->st2 & 0x20) || (s->st1 & 0x20) ? CRC_ERROR : 0) | (s->st2 & 0x40 ? RECORD_TYPE : 0);
    }
    if (disk->multiple && !error) {
        disk->sector++;
        if (find_sector(disk)) {
            locate_sector(disk, cycles);
            return;
        }
    }
    finish(disk, error);
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
    disk->cylinder = cylinder < 0 ? 0 : cylinder >= CYLINDERS ? CYLINDERS - 1 : cylinder;
    if (type == 1) {
        disk->track = disk->data;
    } else if (type == 0 || (type & 1)) {
        disk->track = disk->cylinder;
    }
    disk->op = SEEK;
    disk->ready_at = cycles + COMMAND_DELAY;
}

/* The ID of the sector passing under the head (the track's own order) */
static void read_address(disk_t *disk, uint32_t cycles)
{
    const track_t *t = head_track(disk);
    if (!t) {
        finish(disk, NOT_FOUND);
        return;
    }
    unsigned k = (cycles % REVOLUTION) / (REVOLUTION / t->count);
    if (k >= t->count) {
        k = t->count - 1u;
    }
    const sector_t *s = &disk->layout->sectors[t->first + k];
    uint8_t address[] = {0xa1, 0xa1, 0xa1, 0xfe, s->c, s->h, s->r, s->n};
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
    disk->sector = s->c;                /* the 1772 puts the ID's track there */
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
    if (disk->op == READ_ADDRESS && ready(disk, cycles)) {
        disk->data_deadline = cycles + DATA_TIMEOUT;
        disk->data = disk->buffer[disk->offset++];
        if (disk->offset == disk->size) {
            finish(disk, 0);
        }
    } else if (disk->op == READ_SECTOR && ready(disk, cycles)) {
        disk->data_deadline = cycles + DATA_TIMEOUT;
        disk->data = disk->buffer[disk->offset++ % CHUNK];
        if (disk->offset == disk->size) {
            next_sector(disk, cycles);
        } else if (disk->offset % CHUNK == 0 && !fill(disk)) {
            finish(disk, NOT_FOUND);
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
            disk->buffer[disk->offset++ % CHUNK] = value;
            if (disk->offset == disk->size || disk->offset % CHUNK == 0) {
                if (!flush(disk)) {
                    finish(disk, WRITE_ERROR);
                } else if (disk->offset == disk->size) {
                    next_sector(disk, cycles);
                }
            }
        }
        break;
    }
}
