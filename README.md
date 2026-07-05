This repo is the source code of QEMU and the default branch is set to `stable-11.0`.

## Scope of the custom work

A simple MMIO character-display device (`VGA-Text`, 80x30) is added to the RISC-V `virt` board. It renders its text buffer to a character backend (a terminal, e.g. the one reached over SSH) using ANSI escape sequences. Only the RISC-V softmmu targets are built — no x86/ARM, no graphics (SDL/GTK disabled).

```
  ┌─────────────┐ ┌─────────────┐
  │tty for UART │ │ tty for VGA │
  │    rx/tx    │ │   display   │
  └─────────────┘ └─────────────┘
         ▲               ▲
         │               │
         ▼               │
  ┌─────────────┐ ┌─────────────┐
┌─┤    UART     ├─┤  VGA-Text   ├─┐
│ └─────────────┘ └─────────────┘ │
│                                 │
│       qemu-system-riscv32       │
│                                 │
└─────────────────────────────────┘
```

Files that make up the feature:

| File | Role |
| --- | --- |
| `hw/display/vga_text.c` | The device implementation (QOM type `vga-text`). |
| `hw/display/meson.build` | Adds `vga_text.c` to `system_ss`. |
| `include/hw/riscv/virt.h` | Declares the `VIRT_VGA_TEXT` memmap slot. |
| `hw/riscv/virt.c` | Reserves the memmap entry and instantiates the device. |

## Download

Download [latest release](https://github.com/michaelliao/qemu/releases/latest) or use script [update-qemu.sh](download/update-qemu.sh) to download latest release, unzip and create symbol link at `~/.local/bin/qemu-system-riscv32-vga`.

## Build

Only the RISC-V softmmu targets are built. Graphics UIs are disabled so the device uses a plain character backend.

Environment setup:

```sh
sudo apt install -y git build-essential python3 python3-pip python3-venv \
                    ninja-build pkg-config libglib2.0-dev libpixman-1-dev \
                    flex bison libncurses5-dev libncursesw5-dev
```

Build command:

```sh
mkdir build && cd build
../configure --target-list=riscv64-softmmu,riscv32-softmmu \
             --enable-curses \
             --enable-debug \
             --disable-sdl \
             --disable-gtk \
             --disable-werror
ninja qemu-system-riscv64 qemu-system-riscv32
```

The GitHub action [release.yml](.github/workflows/release.yml) contains the complete build script.

## Running

The device is auto-created by the `virt` machine and mapped at `0x10010000`. For its output backend it looks, in order, for:

1. a chardev explicitly named **`vgaterm`** (`-chardev ...,id=vgaterm`);
2. otherwise the **second** serial line (`serial_hd(1)`).

Using a dedicated `vgaterm` chardev keeps the ANSI output independent of the serial lines (UART0 console + any `-serial`). If neither backend is found the device still maps but produces no output and prints a warning at startup.

Open an SSH terminal as VGA output before start QEMU:

```sh
# start_vga.sh
@echo 'Use current terminal as VGA output...'
@rm -f ./vga.tty
@echo "$(shell tty)" > ./vga.tty
@clear
@trap 'stty echo; echo -e "\n[INFO] Terminal echo restored."' EXIT INT TERM; \
stty -echo; \
cat
```

Then start QEMU:

```sh
# start_qemu.sh
@if [ ! -f ./vga.tty ]; then \
    echo "ERROR: Please create a tty for VGA first!"; \
    exit 1; \
fi
@echo 'Press Ctrl-C to exit QEMU.'
@VGA_TARGET=$$(cat ./vga.tty); \
echo "Launching QEMU... Sending VGA to $$VGA_TARGET"; \
/path/to/qemu-system-riscv32 -machine virt -smp 1 -bios none -semihosting -m 32M \
    -global virtio-mmio.force-legacy=false \
    -drive file=/path/to/disk.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -display none \
    -chardev file,id=uart0,path=/dev/stdout,input-path=/dev/stdin \
    -serial chardev:uart0 \
    -chardev file,id=vgaterm,path=$$VGA_TARGET
```

## Device programming model

MMIO window: `0x1400` bytes at base `0x10010000`.

Registers (all little/native endian, byte-addressable):

| Offset | Name | Access | Meaning |
| --- | --- | --- | --- |
| `0x000` | `CURSOR_X` | R/W | Cursor column (0..79) |
| `0x004` | `CURSOR_Y` | R/W | Cursor row (0..29) |
| `0x008` | `COLOR` | R/W | Default color attribute |
| `0x00C` | `STATUS` | RO | Bit0 READY, Bit1 UPDATED (cleared on read) |
| `0x010` | `RESET` | WO | Clear buffer + cursor, full redraw |
| `0x014` | `START_LINE` | R/W | Top visible buffer row (0..29); hardware scroll |
| `0x100` | `BUFFER` | R/W | 4800-byte text buffer |

Text buffer layout: `80 * 30` cells, 2 bytes each — byte 0 = ASCII character, byte 1 = VGA attribute (low nibble = foreground, high nibble = background, with the usual intensity bit). The buffer spans `[0x100, 0x13C0)`, so the MMIO window is sized `0x1400` (must stay in sync with the `VIRT_VGA_TEXT` size in `hw/riscv/virt.c`).

Writes to the buffer trigger an incremental redraw: only changed cells are re-emitted (cursor-positioned + colored) to the backend, tracked against `old_buffer`.

`START_LINE` implements hardware-style scrolling: the buffer is treated as a ring of 30 rows and physical screen row `r` displays buffer row `(start_line + r) % 30`. Scrolling therefore only updates this register — no buffer data is moved. Changing it maps every cell to a new screen position, so it forces a full redraw; the cursor (`CURSOR_Y`) is a logical buffer row and is mapped through the same offset.
