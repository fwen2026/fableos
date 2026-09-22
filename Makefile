CC := x86_64-elf-gcc
LD := x86_64-elf-ld
QEMU := qemu-system-x86_64
XORRISO := xorriso

BUILD  := build
KERNEL := $(BUILD)/fableos.elf
ISO := $(BUILD)/fableos.iso
ISO_ROOT := $(BUILD)/iso_root

# Point this at Limine's built binaries (the source checkout alone is not enough).
LIMINE_DIR ?= limine
LIMINE := $(LIMINE_DIR)/limine
LIMINE_FILES := $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin

CPPFLAGS := -Iinclude
CFLAGS := -std=c17 -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Wextra -Werror -O2 -g
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -z max-page-size=0x1000

SRCS := $(shell find kernel io -type f -name '*.c' | sort)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all run clean
.DELETE_ON_ERROR:

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld Makefile
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Boot through Limine's BIOS CD image using QEMU's default PC firmware.
$(ISO): $(KERNEL) limine.conf $(LIMINE_FILES) $(LIMINE) Makefile
	@command -v $(XORRISO) >/dev/null || { echo "Missing xorriso; install it to build the boot ISO." >&2; exit 1; }
	@mkdir -p $(ISO_ROOT)/boot/limine
	cp $(KERNEL) $(ISO_ROOT)/boot/fableos.elf
	cp limine.conf $(LIMINE_FILES) $(ISO_ROOT)/boot/limine/
	$(XORRISO) -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table $(ISO_ROOT) -o $@
	$(LIMINE) bios-install $@

$(LIMINE_FILES) $(LIMINE):
	@echo "Missing $@; build/install Limine binaries and set LIMINE_DIR to their directory." >&2
	@exit 1

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -boot d -serial stdio -display none

clean:
	rm -rf $(BUILD)

-include $(DEPS)
