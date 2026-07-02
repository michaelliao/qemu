/*
 * QEMU VGA Text Mode Device
 * 80x25 character display with MMIO mapping for QEMU 11.0
 * Output to console/SSH terminal via Chardev with incremental rendering
 * 
 * QEMU development env:
 * 
 * sudo apt install -y git build-essential python3 python3-pip python3-venv \
 *                     ninja-build pkg-config libglib2.0-dev libpixman-1-dev \
 *                     flex bison libncurses5-dev libncursesw5-dev
 * 
 * mkdir build && cd build
 * ../configure --target-list=riscv32-softmmu \
 *              --enable-curses \
 *              --enable-debug \
 *              --disable-sdl \
 *              --disable-gtk \
 *              --disable-werror
 * ninja
 *
 * Usage:
 *
 * ./qemu-system-riscv32 -machine virt \
 *                       -m 128M \
 *                       -global virtio-mmio.force-legacy=false \
 *                       -drive file=$(DISK),if=none,format=raw,id=x0 \
 *                       -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
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
#define VGA_ROWS            25
#define VGA_BUFFER_SIZE     (VGA_COLS * VGA_ROWS * 2)  /* 80*25*2 = 4000 bytes */

/*
 * MMIO window must cover the control registers (0x000-0x0FF) plus the text
 * buffer that starts at 0x100, i.e. 0x100 + 4000 = 0x10A0. Round up to
 * 0x1100 so the whole buffer is addressable; this matches the size reserved
 * for VIRT_VGA_TEXT in hw/riscv/virt.c.
 */
#define VGA_TEXT_MMIO_SIZE   0x1100

/* VGA Text Mode Registers (MMIO offsets) */
#define VGA_REG_CURSOR_X     0x00    /* Cursor X position (R/W) */
#define VGA_REG_CURSOR_Y     0x04    /* Cursor Y position (R/W) */
#define VGA_REG_COLOR        0x08    /* Default color attribute (R/W) */
#define VGA_REG_STATUS       0x0C    /* Status register (RO) */
#define VGA_REG_RESET        0x10    /* Reset display (WO) */
#define VGA_REG_START_LINE   0x14    /* Top visible buffer row, 0..23 (R/W) */
#define VGA_REG_BUFFER_START 0x100   /* Character buffer start */

/* Highest accepted VGA_REG_START_LINE value (hardware-scroll range 0..23) */
#define VGA_START_LINE_MAX   23

/* Status bits */
#define VGA_STATUS_READY     0x01
#define VGA_STATUS_UPDATED   0x02

struct VGATextState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    Chardev *chr;
    
    uint64_t base_addr;

    /* VGA 核心状态 */
    uint8_t buffer[VGA_BUFFER_SIZE];
    uint8_t old_buffer[VGA_BUFFER_SIZE];
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t default_color;
    uint32_t status;
    uint32_t start_line;   /* buffer row shown at the top of the screen */

    int need_update;
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

/* Convert VGA attributes to compatible ANSI color stream */
static void vga_text_emit_ansi_color(VGATextState *s, uint8_t attr, bool fg)
{
    uint8_t color = fg ? (attr & 0x0F) : ((attr >> 4) & 0x0F);
    uint8_t ansi_code;
    
    if (fg && (attr & 0x08)) {
        color &= 0x07;
        ansi_code = 90 + color;
    } else if (!fg && (color & 0x08)) {
        color &= 0x07;
        ansi_code = 100 + color;
    } else {
        static const uint8_t ansi_map[8] = { 30, 31, 32, 33, 34, 35, 36, 37 };
        ansi_code = fg ? ansi_map[color & 0x07] : (40 + (color & 0x07));
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
    
    /* Restore cursor and global attributes (cursor row mapped like the text) */
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\033[0m\033[%d;%dH",
                       vga_text_screen_row(s, s->cursor_y) + 1, s->cursor_x + 1);
    qemu_chr_write_all(s->chr, (uint8_t *)buf, len);
    
    if (changed) {
        s->status |= VGA_STATUS_UPDATED;
    }
    s->need_update = 0;
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
        memset(s->buffer, 0, sizeof(s->buffer));
        memset(s->old_buffer, 0, sizeof(s->old_buffer));
        s->cursor_x = 0;
        s->cursor_y = 0;
        s->start_line = 0;
        s->status = VGA_STATUS_READY;
        vga_text_update_display(s, true);
        break;

    case VGA_REG_START_LINE:
        /*
         * Scroll by changing which buffer row maps to the top of the screen.
         * The mapping of every cell changes, so a full redraw is required.
         */
        if (val <= VGA_START_LINE_MAX && s->start_line != val) {
            s->start_line = val;
            vga_text_update_display(s, true);
        }
        break;

    default:
        if (addr >= VGA_REG_BUFFER_START && 
            addr < VGA_REG_BUFFER_START + VGA_BUFFER_SIZE) {
            int offset = addr - VGA_REG_BUFFER_START;
            if (offset < VGA_BUFFER_SIZE) {
                if (s->buffer[offset] != (val & 0xFF)) {
                    s->buffer[offset] = val & 0xFF;
                    s->need_update = 1;
                }
            }
        } else {
            qemu_log_mask(LOG_UNIMP, "vga-text: Unimplemented write at 0x%" HWADDR_PRIx "\n", addr);
        }
        break;
    }
    
    if (s->need_update) {
        vga_text_update_display(s, false);
    } else if (cursor_moved && s->chr) {
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
    case VGA_REG_STATUS: {
        uint64_t ret = s->status;
        s->status &= ~VGA_STATUS_UPDATED;
        return ret;
    }
    default:
        if (addr >= VGA_REG_BUFFER_START && addr < VGA_REG_BUFFER_START + VGA_BUFFER_SIZE) {
            int offset = addr - VGA_REG_BUFFER_START;
            return s->buffer[offset];
        }
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
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

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
    
    memset(s->buffer, 0, sizeof(s->buffer));
    memset(s->old_buffer, 0, sizeof(s->old_buffer));
    s->cursor_x = 0;
    s->cursor_y = 0;
    s->start_line = 0;
    s->default_color = 0x07;
    s->status = VGA_STATUS_READY;
    s->need_update = 0;
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
    dc->desc = "VGA Text Mode Device (80x25) for QEMU";
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
