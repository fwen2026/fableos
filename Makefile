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
LIMINE_FILES := $(LIMINE_DIR)/limine-bios.sys \
                $(LIMINE_DIR)/limine-bios-cd.bin \
                $(LIMINE_DIR)/limine-uefi-cd.bin
LIMINE_EFI := $(LIMINE_DIR)/BOOTX64.EFI

CPPFLAGS := -Iinclude
CFLAGS := -std=c17 -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Wextra -Werror -O2 -g
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -z max-page-size=0x1000
# Compiler runtime (__popcountdi2 etc). Query it with the kernel CFLAGS so the
# right multilib is picked; the linker only pulls in the objects we reference.
LIBGCC := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)

SRCS := $(shell find src -type f -name '*.c' | sort)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all run test clean
.DELETE_ON_ERROR:

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld Makefile
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Build Limine's documented BIOS/UEFI hybrid ISO for x86-64.
$(ISO): $(KERNEL) limine.conf $(LIMINE_FILES) $(LIMINE_EFI) $(LIMINE) Makefile
	@command -v $(XORRISO) >/dev/null || { echo "Missing xorriso; install it to build the boot ISO." >&2; exit 1; }
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/fableos.elf
	cp limine.conf $(LIMINE_FILES) $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_EFI) $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI
	$(XORRISO) -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $@
	$(LIMINE) bios-install $@

$(LIMINE_FILES) $(LIMINE_EFI) $(LIMINE):
	@echo "Missing $@; build/install Limine binaries and set LIMINE_DIR to their directory." >&2
	@exit 1

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -boot d -serial stdio -display none

# Host-side unit tests. The kernel's mem* functions are renamed to fable_*
# so they don't collide with the host libc's.
HOSTCC ?= cc
TEST_BUILD := $(BUILD)/test
TEST_RENAME := -Dmemcpy=fable_memcpy -Dmemset=fable_memset \
               -Dmemmove=fable_memmove -Dmemcmp=fable_memcmp
TEST_CFLAGS := -std=c17 -fno-builtin -Wall -Wextra -Werror -O1 -g \
               -fsanitize=address,undefined -fno-omit-frame-pointer

$(TEST_BUILD)/test_memory: test/test_memory.c src/util/string.c include/util/string.h Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_RENAME) $(TEST_CFLAGS) -o $@ test/test_memory.c src/util/string.c

# The kernel's putchar is renamed to fable_putchar so it doesn't collide
# with the host libc's. Kernel sources are compiled on their own with the
# rename; the test files are compiled without it so they can use <stdio.h>.
TEST_PUTCHAR_RENAME := -Dputchar=fable_putchar

$(TEST_BUILD)/console.o: src/kernel/console.c include/kernel/console.h include/io/serial.h Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_PUTCHAR_RENAME) $(TEST_CFLAGS) -c $< -o $@

$(TEST_BUILD)/serial.o: src/io/serial.c include/io/serial.h include/io/io.h Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_PUTCHAR_RENAME) $(TEST_CFLAGS) -c $< -o $@

$(TEST_BUILD)/test_console: test/test_console.c $(TEST_BUILD)/console.o Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/test_console.c $(TEST_BUILD)/console.o

$(TEST_BUILD)/test_console_serial: test/test_console_serial.c $(TEST_BUILD)/console.o $(TEST_BUILD)/serial.o Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/test_console_serial.c $(TEST_BUILD)/console.o $(TEST_BUILD)/serial.o

# pmm.c is #included by each test (via test/memory/pmm_harness.h) to reach its
# statics, so every test is built from a single translation unit.
PMM_TEST_DEPS := test/memory/pmm_harness.h src/memory/pmm.c include/memory/pmm.h Makefile

$(TEST_BUILD)/test_pmm: test/memory/test_pmm.c $(PMM_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/memory/test_pmm.c

$(TEST_BUILD)/test_pmm_check: test/memory/test_pmm_check.c $(PMM_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/memory/test_pmm_check.c

$(TEST_BUILD)/test_pmm_managed: test/memory/test_pmm_managed.c $(PMM_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/memory/test_pmm_managed.c

$(TEST_BUILD)/test_pmm_free_guard: test/memory/test_pmm_free_guard.c $(PMM_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/memory/test_pmm_free_guard.c

# heap.c is #included by its test alongside pmm.c (the heap runs on the real PMM).
$(TEST_BUILD)/test_heap: test/memory/test_heap.c src/memory/heap.c include/memory/heap.h $(PMM_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ test/memory/test_heap.c

.PHONY: test-pmm test-pmm-check test-pmm-managed test-pmm-free-guard test-heap
test-pmm: $(TEST_BUILD)/test_pmm
	./$(TEST_BUILD)/test_pmm

test-pmm-check: $(TEST_BUILD)/test_pmm_check
	./$(TEST_BUILD)/test_pmm_check

test-pmm-managed: $(TEST_BUILD)/test_pmm_managed
	./$(TEST_BUILD)/test_pmm_managed

test-pmm-free-guard: $(TEST_BUILD)/test_pmm_free_guard
	./$(TEST_BUILD)/test_pmm_free_guard

test-heap: $(TEST_BUILD)/test_heap
	./$(TEST_BUILD)/test_heap

test: $(TEST_BUILD)/test_memory $(TEST_BUILD)/test_console $(TEST_BUILD)/test_console_serial $(TEST_BUILD)/test_pmm $(TEST_BUILD)/test_pmm_check $(TEST_BUILD)/test_pmm_managed $(TEST_BUILD)/test_pmm_free_guard $(TEST_BUILD)/test_heap
	./$(TEST_BUILD)/test_memory
	./$(TEST_BUILD)/test_console
	./$(TEST_BUILD)/test_console_serial
	./$(TEST_BUILD)/test_pmm
	./$(TEST_BUILD)/test_pmm_check
	./$(TEST_BUILD)/test_pmm_managed
	./$(TEST_BUILD)/test_pmm_free_guard
	./$(TEST_BUILD)/test_heap

clean:
	rm -rf $(BUILD)

-include $(DEPS)
