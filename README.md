# FableOS

A from-scratch x86-64 hobby kernel written in C, booted with [Limine](https://github.com/limine-bootloader/limine), and developed in QEMU. The long-term goal is a compact OS that can run LLM inference on bare metal.

## Current features

- **Boot:** Limine (BIOS + UEFI hybrid ISO), higher-half kernel, physical memory reached through Limine's higher-half direct map (HHDM).
- **Serial console:** COM1 at 115200 baud, a small `printf`, and leveled logging.
- **Physical memory manager:** buddy allocator for 4 KiB to 2 MiB blocks, using per-order free lists and bitmaps, with an invariant checker (`pmm_check`).
- **Kernel heap:** `kmalloc` / `kfree` with slab caches for 16 to 2048 byte objects and direct page allocation for larger requests. Initialized at boot.

## Future plans

- Interrupts: IDT and exception handlers
- Timer
- Paging and a virtual memory manager
- Scheduler

## Building and running

Requirements:

- `x86_64-elf-gcc` and `x86_64-elf-ld` (cross toolchain)
- `xorriso` and `qemu-system-x86_64`
- Limine's prebuilt binaries in `./limine` (or point `LIMINE_DIR` at them)

```sh
make          # build build/fableos.elf
make run      # build the ISO and boot it in QEMU; serial output goes to the terminal
make test     # build and run the host-side unit tests
make clean
```

## Testing

200+ host-side unit tests cover the console, string utilities, PMM, and heap. They run against a simulated physical memory arena under AddressSanitizer and UndefinedBehaviorSanitizer. I design the allocators and specify what to test; Claude Code writes the test code.
