SHELL := /bin/sh

FPC       ?= fpc
LD        ?= ld
NM        ?= nm
READELF   ?= readelf
XORRISO   ?= xorriso
QEMU      ?= qemu-system-x86_64
LIMINE_DIR ?= ../limine

BUILD_DIR := build
ISO_ROOT  := $(BUILD_DIR)/iso_root
KERNEL_OBJ := $(BUILD_DIR)/mistixkernel.o
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
ISO_IMAGE := $(BUILD_DIR)/mistix-phase0.iso

.PHONY: all kernel verify iso run clean check-tools

all: kernel verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

kernel: $(KERNEL_ELF)

$(KERNEL_OBJ): src/kernel/mistixkernel.pas | $(BUILD_DIR)
	$(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU$(BUILD_DIR) $<
	@test -f $(KERNEL_OBJ) || { echo "FPC did not create $(KERNEL_OBJ)"; exit 1; }

$(KERNEL_ELF): $(KERNEL_OBJ) linker.ld
	@set -eu; \
	UNDEFINED="$$($(NM) -u $(KERNEL_OBJ) 2>/dev/null || true)"; \
	if [ -n "$$UNDEFINED" ]; then \
	  echo "Unexpected unresolved symbols in the freestanding Pascal object:"; \
	  echo "$$UNDEFINED"; \
	  echo "The kernel must not depend on the host/FPC runtime."; \
	  exit 1; \
	fi
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 \
	  -T linker.ld -o $(KERNEL_ELF) $(KERNEL_OBJ)

verify: $(KERNEL_ELF)
	@$(READELF) -h $(KERNEL_ELF) | grep -q 'ELF64' || { echo 'Not an ELF64 image'; exit 1; }
	@$(NM) -u $(KERNEL_ELF) 2>/dev/null | grep -q . && { echo 'Kernel has unresolved symbols'; exit 1; } || true
	@$(NM) $(KERNEL_ELF) | grep -q ' _start$$' || { echo 'Missing _start entry point'; exit 1; }
	@echo 'Kernel ELF verification passed.'
	@$(READELF) -h $(KERNEL_ELF) | grep -E 'Entry point|エントリポイント'

check-tools:
	@command -v $(FPC) >/dev/null || { echo 'Missing Free Pascal Compiler (fpc)'; exit 1; }
	@command -v $(LD) >/dev/null || { echo 'Missing GNU ld'; exit 1; }
	@command -v $(NM) >/dev/null || { echo 'Missing nm (binutils)'; exit 1; }
	@command -v $(READELF) >/dev/null || { echo 'Missing readelf (binutils)'; exit 1; }
	@command -v $(XORRISO) >/dev/null || { echo 'Missing xorriso'; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo 'Missing qemu-system-x86_64'; exit 1; }
	@test -x "$(LIMINE_DIR)/bin/limine" || { echo 'Set LIMINE_DIR to a built Limine v12 tree'; exit 1; }

iso: $(KERNEL_ELF)
	@test -x "$(LIMINE_DIR)/bin/limine" || { echo 'Set LIMINE_DIR to a built Limine v12 tree'; exit 1; }
	@test -f "$(LIMINE_DIR)/bin/limine-bios.sys" || { echo 'Missing Limine BIOS stage 2'; exit 1; }
	@test -f "$(LIMINE_DIR)/bin/limine-bios-cd.bin" || { echo 'Missing Limine BIOS CD image'; exit 1; }
	@test -f "$(LIMINE_DIR)/bin/limine-uefi-cd.bin" || { echo 'Missing Limine UEFI CD image'; exit 1; }
	@test -f "$(LIMINE_DIR)/bin/BOOTX64.EFI" || { echo 'Missing Limine BOOTX64.EFI'; exit 1; }
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL_ELF) $(ISO_ROOT)/boot/kernel.elf
	cp boot/limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/bin/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/bin/limine-bios-cd.bin $(ISO_ROOT)/boot/
	cp $(LIMINE_DIR)/bin/limine-uefi-cd.bin $(ISO_ROOT)/boot/
	cp $(LIMINE_DIR)/bin/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI
	$(XORRISO) -as mkisofs -R -r -J \
	  -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	  --protective-msdos-label $(ISO_ROOT) -o $(ISO_IMAGE)
	$(LIMINE_DIR)/bin/limine bios-install $(ISO_IMAGE)
	@echo 'Created $(ISO_IMAGE)'

run: iso
	$(QEMU) -M q35 -m 256M -smp 1 -cdrom $(ISO_IMAGE) -boot d \
	  -serial stdio -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD_DIR) *.o *.ppu
