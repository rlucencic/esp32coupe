/* SAM Coupé ASIC, using the shared Z80 core. */
#include "coupe.h"
#include "coupe_lock.h"
#include "floppy.h"
#include "sound.h"
#include "z80.h"

#include <stdatomic.h>
#include <string.h>

enum {
    PAGE = 16384,
    LINE_CYCLES = 384,
    FRAME_CYCLES = 312 * LINE_CYCLES,
    FIRST_SCREEN_LINE = 68,
    TOP = FIRST_SCREEN_LINE - 24,
    SIDE = 32,
    IRQ_CYCLES = 128,
};

enum {
    CLUT = 0xf8, STATUS = 0xf9, LMPR = 0xfa, HMPR = 0xfb,
    VMPR = 0xfc, KEYBOARD = 0xfe, ATTRIBUTE = 0xff,
};

static coupe_config_t cfg;
static uint8_t lmpr, hmpr, vmpr, border, irq_line, display_bus;
static uint16_t colours[16], palette[128];
static uint8_t clut[16];                /* (the web page's) */
static const uint8_t open_bus[PAGE] = {[0 ... PAGE - 1] = 0xff};
static _Atomic uint8_t keys[9];
static _Atomic uint32_t frames, signature, cpu_status;
static coupe_lock_t picture_lock = COUPE_LOCK_INIT;
static int front;
static uint32_t frame_clock;

static uint8_t *read_page(unsigned page)
{
    return page < cfg.ram_size / PAGE ? cfg.ram + page * PAGE : (uint8_t *)open_bus;
}

static void map_memory(void)
{
    const unsigned low = lmpr & 31;
    const unsigned high = hmpr & 31;
    const unsigned pages[] = {low, (low + 1) & 31, high, (high + 1) & 31};
    for (unsigned i = 0; i < 4; i++) {
        z80_rd[i] = read_page(pages[i]);
        z80_wr[i] = z80_rd[i] == open_bus ? NULL : z80_rd[i];
    }
    if (!(lmpr & 0x20)) {
        z80_rd[0] = (uint8_t *)cfg.rom;
        z80_wr[0] = NULL;
    } else if (lmpr & 0x80) {
        z80_wr[0] = NULL;
    }
    if (hmpr & 0x80) {
        z80_rd[2] = z80_rd[3] = (uint8_t *)open_bus;
        z80_wr[2] = z80_wr[3] = NULL;
    }
    if (lmpr & 0x40) {
        z80_rd[3] = (uint8_t *)cfg.rom + PAGE;
        z80_wr[3] = NULL;
    }
#if Z80_CONTENTION
    for (unsigned i = 0; i < 4; i++) {
        z80_contended[i] = z80_rd[i] != open_bus && z80_rd[i] != cfg.rom &&
                           z80_rd[i] != cfg.rom + PAGE;
    }
    z80_contention = (vmpr & 0x40) && (border & 0x80) ? NULL : cfg.contention;
#endif
}

static uint8_t keyboard_row(uint8_t select)
{
    if (select == 0xff) {
        return atomic_load_explicit(&keys[8], memory_order_relaxed);
    }
    uint8_t value = 0xff;
    for (unsigned row = 0; row < 8; row++) {
        if (!(select & (1u << row))) {
            value &= atomic_load_explicit(&keys[row], memory_order_relaxed);
        }
    }
    return value;
}

static uint8_t interrupt_status(int t)
{
    uint8_t status = 0x1f;
    if (t >= 0 && t < IRQ_CYCLES) {
        status &= ~0x08;
    }
    int line_time = (FIRST_SCREEN_LINE + irq_line) * LINE_CYCLES;
    if (irq_line < 192 && t >= line_time && t < line_time + IRQ_CYCLES) {
        status &= ~0x01;
    }
    return status;
}

static uint8_t port_in(uint16_t port, int at)
{
    int time = (z80.t + at) % FRAME_CYCLES;
    switch (port & 0xff) {
    case STATUS:
        return (keyboard_row(port >> 8) & 0xe0) | interrupt_status(time);
    case LMPR: return lmpr;
    case HMPR: return hmpr;
    case VMPR: return vmpr | 0x80;
    case KEYBOARD: return (keyboard_row(port >> 8) & 0x1f) | (border & 0x80);
    case ATTRIBUTE: return display_bus;
    case CLUT: return (port & 0x100) ? time / LINE_CYCLES - FIRST_SCREEN_LINE
                                    : (time % LINE_CYCLES) & 0xfc;
    default:
        if ((port & 0xe8) == 0xe0) {
            return coupe_fdc_read((port >> 4) & 1, port & 7, frame_clock + z80.t + at);
        }
        return 0xff;
    }
}

static void port_out(uint16_t port, uint8_t value, int at)
{
    switch (port & 0xff) {
    case CLUT:
        colours[(port >> 8) & 15] = palette[value & 127];
        clut[(port >> 8) & 15] = value & 127;
        break;
    case STATUS: irq_line = value; break;
    case LMPR: lmpr = value; map_memory(); break;
    case HMPR: hmpr = value; map_memory(); break;
    case VMPR: vmpr = value & 0x7f; map_memory(); break;
    case KEYBOARD:
        if ((border ^ value) & 0x10) {
            coupe_sound_advance(z80.t + at);
            coupe_sound_beeper((value & 0x10) != 0);
        }
        border = value;
        map_memory();
        break;
    case ATTRIBUTE:  /* the SAA1099 occupies the output side of this port */
        coupe_sound_advance(z80.t + at);
        if (port & 0x100) {
            coupe_sound_select(value);
        } else {
            coupe_sound_write(value);
        }
        break;
    default:
        if ((port & 0xe8) == 0xe0) {
            coupe_fdc_write((port >> 4) & 1, port & 7, value, frame_clock + z80.t + at);
        }
        break;
    }
}

static uint16_t rgb565(unsigned value)
{
    unsigned half = (value >> 3) & 1;
    unsigned red = ((value >> 3) & 4) | (value & 2) | half;
    unsigned green = ((value >> 4) & 4) | ((value >> 1) & 2) | half;
    unsigned blue = ((value >> 2) & 4) | ((value << 1) & 2) | half;
    return (red * 31 / 7 << 11) | (green * 63 / 7 << 5) | (blue * 31 / 7);
}

static void draw_line(uint16_t *out, int y, uint32_t frame)
{
    int mode = vmpr >> 5;
    uint16_t edge = colours[(border & 7) | ((border >> 2) & 8)];
    bool active = y >= 0 && y < 192;
    bool blank = mode >= 2 && (border & 0x80);
    for (int x = 0; x < SIDE; x++) {
        out[x] = out[COUPE_WIDTH - 1 - x] = blank ? 0 : edge;
    }
    out += SIDE;
    if (!active || blank) {
        for (int x = 0; x < 512; x++) {
            out[x] = blank ? 0 : edge;
        }
        return;
    }

    if (mode < 2) {
        const uint8_t *screen = read_page(vmpr & 31);
        int bitmap = mode == 0 ? ((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)
                               : y * 32;
        int attrs = mode == 0 ? 6144 + (y / 8) * 32 : 8192 + bitmap;
        for (int cell = 0; cell < 32; cell++) {
            uint8_t bits = screen[bitmap + cell];
            uint8_t attr = screen[attrs + cell];
            unsigned bright = (attr >> 3) & 8;
            uint16_t ink = colours[(attr & 7) | bright];
            uint16_t paper = colours[((attr >> 3) & 7) | bright];
            bool flash = (attr & 0x80) && (frame & 16);
            for (unsigned bit = 0x80; bit; bit >>= 1) {
                uint16_t pixel = ((bits & bit) != 0) != flash ? ink : paper;
                *out++ = pixel;
                *out++ = pixel;
            }
            display_bus = attr;
        }
    } else {
        static const uint8_t mode3_index[] = {0, 2, 1, 3};
        unsigned base = (hmpr >> 3) & 12;
        unsigned offset = y * 128;
        const uint8_t *line = read_page((vmpr & 30) + offset / PAGE) + offset % PAGE;
        for (int byte = 0; byte < 128; byte++) {
            uint8_t bits = line[byte];
            if (mode == 2) {
                *out++ = colours[base | mode3_index[bits >> 6]];
                *out++ = colours[base | mode3_index[(bits >> 4) & 3]];
                *out++ = colours[base | mode3_index[(bits >> 2) & 3]];
                *out++ = colours[base | mode3_index[bits & 3]];
            } else {
                *out++ = colours[bits >> 4];
                *out++ = colours[bits >> 4];
                *out++ = colours[bits & 15];
                *out++ = colours[bits & 15];
            }
            if ((byte & 3) == 2) {
                display_bus = bits;
            }
        }
    }
}

void coupe_init(const coupe_config_t *config)
{
    coupe_lock(&picture_lock);
    cfg = *config;
    coupe_unlock(&picture_lock);
    for (unsigned i = 0; i < 128; i++) {
        palette[i] = rgb565(i);
    }
#if Z80_CONTENTION
    memset(cfg.contention, 0, COUPE_TIMING_BYTES);
    /* Approximate the active-display memory slots on an eight-cycle grid. */
    for (int line = FIRST_SCREEN_LINE; line < FIRST_SCREEN_LINE + 192; line++) {
        for (int t = 128; t < LINE_CYCLES; t++) {
            cfg.contention[line * LINE_CYCLES + t] = (8 - (t & 7)) & 7;
        }
    }
#endif
    static const z80_bus_t bus = {.in = port_in, .out = port_out};
    z80_set_bus(&bus);
    coupe_reset();
}

void coupe_reset(void)
{
    memset(cfg.ram, 0, cfg.ram_size);
    lmpr = hmpr = vmpr = border = 0;
    irq_line = 255;
    display_bus = 0xff;
    frame_clock = 0;
    memset(colours, 0, sizeof(colours));
    for (unsigned i = 0; i < 9; i++) {
        atomic_store_explicit(&keys[i], 0xff, memory_order_relaxed);
    }
    z80_reset();
    z80.t = 0;
    map_memory();
    coupe_fdc_reset();
    coupe_sound_reset(cfg.audio);
    atomic_store(&frames, 0);
    atomic_store(&cpu_status, 0);
}

void coupe_run_frame(void)
{
    uint32_t frame = atomic_load_explicit(&frames, memory_order_relaxed);
    uint16_t *back = cfg.pictures + (1 - front) * COUPE_FRAME_PIXELS;
    for (int line = 0; line < 312; line++) {
        int until = (line + 1) * LINE_CYCLES;
        while (z80.t < until) {
            z80.int_line = interrupt_status(z80.t) != 0x1f;
            int elapsed = z80_step();
            if (elapsed <= 0) {
                return;
            }
            z80.t += elapsed;
        }
        if (line >= TOP && line < TOP + COUPE_HEIGHT) {
            draw_line(back + (line - TOP) * COUPE_WIDTH, line - FIRST_SCREEN_LINE, frame);
        }
    }
    coupe_sound_finish_frame();
    z80.t -= FRAME_CYCLES;
    frame_clock += FRAME_CYCLES;
    const uint16_t *previous = cfg.pictures + front * COUPE_FRAME_PIXELS;
    if (memcmp(back, previous, COUPE_FRAME_PIXELS * sizeof(*back))) {
        coupe_lock(&picture_lock);
        front = 1 - front;
        atomic_fetch_add(&signature, 1);
        coupe_unlock(&picture_lock);
    }
    atomic_store_explicit(&cpu_status, z80.pc.w | (uint32_t)(1 + (vmpr >> 5)) << 16, memory_order_relaxed);
    atomic_store_explicit(&frames, frame + 1, memory_order_relaxed);
}

void coupe_render_line(int y, uint16_t *out)
{
    coupe_lock(&picture_lock);
    memcpy(out, cfg.pictures + front * COUPE_FRAME_PIXELS + y * COUPE_WIDTH,
           COUPE_WIDTH * sizeof(*out));
    coupe_unlock(&picture_lock);
}

uint32_t coupe_screen_signature(void)
{
    return atomic_load_explicit(&signature, memory_order_relaxed);
}

void coupe_get_status(coupe_status_t *status)
{
    uint32_t state = atomic_load_explicit(&cpu_status, memory_order_relaxed);
    *status = (coupe_status_t){.frames = atomic_load_explicit(&frames, memory_order_relaxed),
                               .pc = state & 0xffff, .mode = state >> 16};
    status->lmpr = lmpr;
    status->hmpr = hmpr;
    status->vmpr = vmpr;
    status->border = border;
    status->line_irq = irq_line;
    memcpy(status->clut, clut, sizeof(clut));
}

/* USB usage values indexed by the SAM's nine active-low keyboard rows. */
void coupe_hid_key(uint8_t usage, bool pressed)
{
    static const uint8_t matrix[9][8] = {
        {0xe1, 0x1d, 0x1b, 0x06, 0x19, 0x3a, 0x3b, 0x3c},
        {0x04, 0x16, 0x07, 0x09, 0x0a, 0x3d, 0x3e, 0x3f},
        {0x14, 0x1a, 0x08, 0x15, 0x17, 0x40, 0x41, 0x42},
        {0x1e, 0x1f, 0x20, 0x21, 0x22, 0x29, 0x2b, 0x39},
        {0x27, 0x26, 0x25, 0x24, 0x23, 0x2d, 0x57, 0x2a},
        {0x13, 0x12, 0x0c, 0x18, 0x1c, 0x2e, 0x34, 0x43},
        {0x28, 0x0f, 0x0e, 0x0d, 0x0b, 0x33, 0x32, 0xe6},
        {0x2c, 0xe2, 0x10, 0x11, 0x05, 0x36, 0x37, 0x35},
        {0xe0, 0x52, 0x51, 0x50, 0x4f, 0, 0, 0},
    };
    if (usage == 0) {
        return;
    }
    if (usage == 0xe5 || usage == 0xe4 || usage == 0x4c || usage == 0x58) {
        usage = usage == 0xe5 ? 0xe1 : usage == 0xe4 ? 0xe0 : usage == 0x4c ? 0x2a : 0x28;
    }
    for (unsigned row = 0; row < 9; row++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            if (usage == matrix[row][bit]) {
                if (pressed) {
                    atomic_fetch_and_explicit(&keys[row], (uint8_t)~(1u << bit), memory_order_relaxed);
                } else {
                    atomic_fetch_or_explicit(&keys[row], (uint8_t)(1u << bit), memory_order_relaxed);
                }
                return;
            }
        }
    }
}
