CROSS  ?= x86_64-elf-
CC     := $(CROSS)gcc
LD     := $(CROSS)ld

BUILD  := build
KERNEL := $(BUILD)/fableos.elf

# -m32 keeps us in 32-bit protected mode, where Multiboot hands off.
CFLAGS := -std=c17 -m32 -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pic -mno-red-zone \
          -Wall -Wextra -Werror -O2 -g -Iinclude
ASFLAGS := -m32 -ffreestanding -g
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

SRCS := $(shell find kernel drivers -name '*.c') $(shell find boot -name '*.S')
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all run clean

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -serial stdio -display none

clean:
	rm -rf $(BUILD)

-include $(DEPS)
