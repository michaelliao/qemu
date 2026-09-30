/*
 * QEMU VGA Text Mode Device
 * 80x30 character display with MMIO mapping for QEMU 11.0
 * Output to console/SSH terminal via Chardev with incremental rendering
 *
 * UMA design: the text buffer (frame buffer, FB) lives in guest main memory.
 * The guest kernel allocates it and programs its address into
 * VGA_REG_FB_ADDR_LO/HI; a 30 Hz refresh timer then copies the FB into the
 * device's shadow buffer and redraws the changed cells.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/target-info.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char-fe.h"
#include "ui/console.h"
#include "qom/object.h"


#define TYPE_VGA_TEXT "vga-text"
OBJECT_DECLARE_SIMPLE_TYPE(VGATextState, VGA_TEXT)

#define VGA_COLS            80
#define VGA_ROWS            25   /* 80x25 text mode */
#define VGA_BUFFER_SIZE     (VGA_COLS * VGA_ROWS * 2)  /* 80*25*2 = 4000 bytes */

/*
 * MMIO window size. Only the control registers (0x000-0x01F) are implemented
 * now that the text buffer lives in guest memory, but the window is kept at
 * 0x100 to match the size reserved for VIRT_VGA_TEXT in hw/riscv/virt.c.
 */
#define VGA_TEXT_MMIO_SIZE   0x100

/* VGA Text Mode Registers (MMIO offsets) */
#define VGA_REG_CURSOR_X     0x00    /* Cursor X position (R/W) */
#define VGA_REG_CURSOR_Y     0x04    /* Cursor Y position (R/W) */
#define VGA_REG_COLOR        0x08    /* Default color attribute (R/W) */
#define VGA_REG_STATUS       0x0C    /* Status register (RO) */
#define VGA_REG_RESET        0x10    /* Reset display (WO) */
#define VGA_REG_START_LINE   0x14    /* Top visible buffer row, 0..29 (R/W) */
#define VGA_REG_FB_ADDR_LO   0x18    /* Frame buffer address, bits 31..0 (R/W) */
#define VGA_REG_FB_ADDR_HI   0x1C    /* Frame buffer address, bits 63..32 (R/W, 0 on rv32) */

/* Refresh timer period: 30 frames per second */
#define VGA_REFRESH_HZ       30
#define VGA_REFRESH_PERIOD_NS (NANOSECONDS_PER_SECOND / VGA_REFRESH_HZ)

/* Highest accepted VGA_REG_START_LINE value (hardware-scroll range 0..29) */
#define VGA_START_LINE_MAX   (VGA_ROWS - 1)

/* Status bits */
#define VGA_STATUS_READY     0x01
#define VGA_STATUS_UPDATED   0x02

struct VGATextState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    Chardev *chr;
    QEMUTimer *refresh_timer;

    uint64_t base_addr;

    /* VGA 核心状态 */
    uint8_t buffer[VGA_BUFFER_SIZE];     /* shadow copy of the guest FB */
    uint8_t old_buffer[VGA_BUFFER_SIZE]; /* what is currently on the terminal */
    uint64_t fb_addr;      /* guest FB address (PA, or VA when MMU is on); 0 = unset */
    CPUState *fb_cpu;      /* CPU whose MMU translates fb_addr */
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t default_color;
    uint32_t status;
    uint32_t start_line;   /* buffer row shown at the top of the screen */

    bool need_full_redraw;
};

/*
 * Map a logical buffer row to the physical screen row, honouring the
 * hardware-scroll start line. The buffer is treated as a ring of VGA_ROWS
 * rows: screen row 0 shows buffer row start_line, and so on with wraparound.
 * This lets the guest scroll by only updating VGA_REG_START_LINE, without
 * moving any data in the text buffer.
 */
static inline int vga_text_screen_row(VGATextState *s, int buf_row)
{
    return (buf_row - (int)s->start_line + VGA_ROWS) % VGA_ROWS;
}

/*
 * Convert one VGA colour nibble to an ANSI SGR colour code and emit it.
 *
 * VGA packs colour bits as (I)(R)(G)(B): bit0=blue, bit1=green, bit2=red,
 * bit3=intensity. ANSI SGR orders its base colours differently: the offset
 * added to 30/40 is bit0=red, bit1=green, bit2=blue. Red and blue are thus
 * swapped relative to VGA, so a direct copy would turn blue into red and vice
 * versa. vga_to_ansi[] performs that swap, e.g. VGA 1 (blue) -> ANSI 4
 * (\033[34m), VGA 4 (red) -> ANSI 1 (\033[31m), VGA 3 (cyan) -> ANSI 6,
 * VGA 6 (yellow) -> ANSI 3. The intensity bit selects the bright range
 * (90/100 instead of 30/40) for both foreground and background.
 */
static void vga_text_emit_ansi_color(VGATextState *s, uint8_t attr, bool fg)
{
    static const uint8_t vga_to_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    uint8_t nibble = fg ? (attr & 0x0F) : ((attr >> 4) & 0x0F);
    uint8_t base = vga_to_ansi[nibble & 0x07];
    bool bright = nibble & 0x08;
    unsigned ansi_code;

    if (fg) {
        ansi_code = (bright ? 90 : 30) + base;
    } else {
        ansi_code = (bright ? 100 : 40) + base;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "\033[%dm", ansi_code);
    qemu_chr_write_all(s->chr, (uint8_t *)buf, len);
}

/* Render a single changed character (precise local refresh) */
static void vga_text_render_char(VGATextState *s, int row, int col)
{
    int offset = (row * VGA_COLS + col) * 2;
    uint8_t ch = s->buffer[offset];
    uint8_t attr = s->buffer[offset + 1];
    int screen_row = vga_text_screen_row(s, row);

    /* cursor positioning (buffer row mapped through the start line) */
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\033[%d;%dH", screen_row + 1, col + 1);
    qemu_chr_write_all(s->chr, (uint8_t *)buf, len);
    
    /* color */
    vga_text_emit_ansi_color(s, attr, true);
    vga_text_emit_ansi_color(s, attr, false);
    
    /* non-visible character handling */
    if (ch == 0) ch = ' ';
    qemu_chr_write_all(s->chr, &ch, 1);
}

/* Display incremental redraw core logic */
static void vga_text_update_display(VGATextState *s, bool full_redraw)
{
    if (!s->chr) {
        return;
    }
    
    if (full_redraw) {
        qemu_chr_write_all(s->chr, (uint8_t *)"\033[2J\033[H", 7);
    }
    
    bool changed = false;
    for (int row = 0; row < VGA_ROWS; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            int idx = (row * VGA_COLS + col) * 2;
            if (full_redraw || 
                s->buffer[idx] != s->old_buffer[idx] || 
                s->buffer[idx + 1] != s->old_buffer[idx + 1]) {
                
                vga_text_render_char(s, row, col);
                s->old_buffer[idx] = s->buffer[idx];
                s->old_buffer[idx + 1] = s->buffer[idx + 1];
                changed = true;
            }
        }
    }
    
    if (changed) {
        /* Restore cursor and global attributes (cursor row mapped like the text) */
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "\033[0m\033[%d;%dH",
                           vga_text_screen_row(s, s->cursor_y) + 1, s->cursor_x + 1);
        qemu_chr_write_all(s->chr, (uint8_t *)buf, len);
        s->status |= VGA_STATUS_UPDATED;
    }
}

/* Fill the shadow buffer with blank cells in the default colour */
static void vga_text_fill_blank(VGATextState *s)
{
    for (int i = 0; i < VGA_BUFFER_SIZE; i += 2) {
        s->buffer[i] = ' ';
        s->buffer[i + 1] = s->default_color;
    }
}

/*
 * Copy the guest frame buffer into the shadow buffer.
 *
 * fb_addr may be a physical address (MMU off) or a virtual address (MMU on),
 * so it is read through the MMU of the CPU that programmed it:
 * cpu_memory_rw_debug() walks that CPU's current page tables, and with
 * translation disabled it degenerates to a physical access. The FB may span
 * a page boundary with non-contiguous physical pages; the debug accessor
 * translates page by page. On a translation fault the previous frame is kept.
 */
static void vga_text_fetch_fb(VGATextState *s)
{
    uint8_t frame[VGA_BUFFER_SIZE];

    if (s->fb_addr == 0 || !s->fb_cpu) {
        vga_text_fill_blank(s);
        return;
    }

    if (cpu_memory_rw_debug(s->fb_cpu, s->fb_addr, frame, sizeof(frame),
                            false) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vga-text: cannot read frame buffer at 0x%" PRIx64 "\n",
                      s->fb_addr);
        return;
    }
    memcpy(s->buffer, frame, sizeof(frame));
}

/* 30 Hz refresh: pull the FB from guest memory and redraw changed cells */
static void vga_text_refresh(void *opaque)
{
    VGATextState *s = opaque;
    bool full = s->need_full_redraw;

    vga_text_fetch_fb(s);
    s->need_full_redraw = false;
    vga_text_update_display(s, full);

    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VGA_REFRESH_PERIOD_NS);
}

/*
 * Latch the FB address. The writing CPU is remembered so a virtual FB
 * address is later translated with that CPU's MMU (the guest kernel's
 * mapping). rv32 has 32-bit addresses, so the high word is forced to 0.
 */
static void vga_text_set_fb_addr(VGATextState *s, uint64_t addr)
{
    if (target_long_bits() == 32) {
        addr &= 0xFFFFFFFFULL;
    }
    s->fb_addr = addr;
    if (current_cpu) {
        s->fb_cpu = current_cpu;
    }
}

/* MMIO Write */
static void vga_text_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VGATextState *s = opaque;
    bool cursor_moved = false; /* is cursor moved only */
    
    switch (addr) {
    case VGA_REG_CURSOR_X:
        if (val < VGA_COLS && s->cursor_x != val) {
            s->cursor_x = val;
            cursor_moved = true;
        }
        break;
        
    case VGA_REG_CURSOR_Y:
        if (val < VGA_ROWS && s->cursor_y != val) {
            s->cursor_y = val;
            cursor_moved = true;
        }
        break;
        
    case VGA_REG_COLOR:
        s->default_color = val & 0xFF;
        break;
        
    case VGA_REG_RESET:
        /*
         * The text buffer belongs to the guest now, so it is not cleared
         * here; reset the cursor/scroll state and redraw the whole screen
         * from the FB on the next refresh tick.
         */
        s->cursor_x = 0;
        s->cursor_y = 0;
        s->start_line = 0;
        s->status = VGA_STATUS_READY;
        s->need_full_redraw = true;
        break;

    case VGA_REG_START_LINE:
        /*
         * Scroll by changing which buffer row maps to the top of the screen.
         * The mapping of every cell changes, so a full redraw is required.
         */
        if (val <= VGA_START_LINE_MAX && s->start_line != val) {
            s->start_line = val;
            s->need_full_redraw = true;
        }
        break;

    case VGA_REG_FB_ADDR_LO:
        vga_text_set_fb_addr(s, deposit64(s->fb_addr, 0, 32, val));
        break;

    case VGA_REG_FB_ADDR_HI:
        vga_text_set_fb_addr(s, deposit64(s->fb_addr, 32, 32, val));
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "vga-text: Unimplemented write at 0x%" HWADDR_PRIx "\n", addr);
        break;
    }

    if (cursor_moved && s->chr) {
        /* Only cursor moved */
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "\033[%d;%dH",
                           vga_text_screen_row(s, s->cursor_y) + 1, s->cursor_x + 1);
        qemu_chr_write_all(s->chr, (uint8_t *)buf, len);
    }
}

/* MMIO Read */
static uint64_t vga_text_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    VGATextState *s = opaque;
    switch (addr) {
    case VGA_REG_CURSOR_X: return s->cursor_x;
    case VGA_REG_CURSOR_Y: return s->cursor_y;
    case VGA_REG_COLOR:    return s->default_color;
    case VGA_REG_START_LINE: return s->start_line;
    case VGA_REG_FB_ADDR_LO: return extract64(s->fb_addr, 0, 32);
    case VGA_REG_FB_ADDR_HI: return extract64(s->fb_addr, 32, 32);
    case VGA_REG_STATUS: {
        uint64_t ret = s->status;
        s->status &= ~VGA_STATUS_UPDATED;
        return ret;
    }
    default:
        break;
    }
    return 0;
}

static const MemoryRegionOps vga_text_mmio_ops = {
    .read = vga_text_mmio_read,
    .write = vga_text_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void vga_text_reset(DeviceState *dev)
{
    VGATextState *s = VGA_TEXT(dev);

    memset(s->old_buffer, 0, sizeof(s->old_buffer));
    s->fb_addr = 0;
    s->fb_cpu = NULL;
    s->cursor_x = 0;
    s->cursor_y = 0;
    s->start_line = 0;
    s->default_color = 0x07;
    s->status = VGA_STATUS_READY;
    s->need_full_redraw = true;
    vga_text_fill_blank(s);
}

static void vga_text_realize(DeviceState *dev, Error **errp)
{
    VGATextState *s = VGA_TEXT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    
    memory_region_init_io(&s->mmio, OBJECT(s), &vga_text_mmio_ops, s,
                          "vga-text-mmio", VGA_TEXT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    if (s->base_addr != 0) {
        sysbus_mmio_map(sbd, 0, s->base_addr);
    }
    
    vga_text_reset(dev);

    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vga_text_refresh, s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VGA_REFRESH_PERIOD_NS);
}

/*
 * Optional MMIO base address. When set (e.g. via -device vga-text,base_addr=..)
 * the device maps itself in realize(). The riscv 'virt' machine leaves this at
 * 0 and maps the device explicitly at VIRT_VGA_TEXT instead.
 */
static const Property vga_text_properties[] = {
    DEFINE_PROP_UINT64("base_addr", VGATextState, base_addr, 0),
};

static void vga_text_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = vga_text_realize;
    device_class_set_legacy_reset(dc, vga_text_reset);
    dc->desc = "VGA Text Mode Device (80x30) for QEMU";
    dc->user_creatable = true;
    device_class_set_props(dc, vga_text_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    object_class_property_add_link(klass, "chardev", TYPE_CHARDEV,
                                   offsetof(VGATextState, chr),
                                   qdev_prop_allow_set_link_before_realize,
                                   OBJ_PROP_LINK_STRONG);
}

static const TypeInfo vga_text_info = {
    .name          = TYPE_VGA_TEXT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VGATextState),
    .class_init    = vga_text_class_init,
};

static void vga_text_register_types(void) { type_register_static(&vga_text_info); }
type_init(vga_text_register_types)
