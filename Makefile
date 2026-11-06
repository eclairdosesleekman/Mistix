SHELL := /bin/sh

FPC        ?= fpc
LD         ?= ld
NM         ?= nm
READELF    ?= readelf
XORRISO    ?= xorriso
QEMU       ?= qemu-system-x86_64
LIMINE_DIR ?= ../limine

BUILD_DIR := build
USER_BUILD_DIR := $(BUILD_DIR)/user
ISO_ROOT  := $(BUILD_DIR)/iso_root
KERNEL_OBJ := $(BUILD_DIR)/mistixkernel.o
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
USER_OBJ := $(USER_BUILD_DIR)/mistixinit.o
USER_ELF := $(USER_BUILD_DIR)/init.elf
USER_BLOB_OBJ := $(BUILD_DIR)/user_init_blob.o
ISO_IMAGE := $(BUILD_DIR)/mistix-phase3.iso
SRC_DIR   := src/kernel
PASCAL_SOURCES := $(SRC_DIR)/mistixkernel.pas \
                  $(SRC_DIR)/mistix_types.inc \
                  $(SRC_DIR)/mistix_console.inc \
                  $(SRC_DIR)/mistix_cpu.inc \
                  $(SRC_DIR)/mistix_memory.inc \
                  $(SRC_DIR)/mistix_threads.inc \
                  $(SRC_DIR)/mistix_capability.inc \
                  $(SRC_DIR)/mistix_ipc.inc \
                  $(SRC_DIR)/mistix_notification.inc \
                  $(SRC_DIR)/mistix_memory_object.inc \
                  $(SRC_DIR)/mistix_address_space.inc \
                  $(SRC_DIR)/mistix_process.inc \
                  $(SRC_DIR)/mistix_syscall.inc \
                  $(SRC_DIR)/mistix_main.inc

.PHONY: all check-source check-tools user user-verify kernel verify iso run run-debug clean

all: check-source user kernel verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(USER_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $(USER_BUILD_DIR)

check-source:
	python3 tools/check_source.py

user: user-verify

$(USER_OBJ): userland/init/mistixinit.pas | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. ../../userland/init/mistixinit.pas
	@test -f $(USER_OBJ) || { echo "FPC did not create $(USER_OBJ)"; exit 1; }

$(USER_ELF): $(USER_OBJ) userland/user.ld
	@set -eu; \
	UNDEFINED="$$($(NM) -u $(USER_OBJ) 2>/dev/null || true)"; \
	if [ -n "$$UNDEFINED" ]; then \
	  echo 'User init object unexpectedly depends on external runtime symbols:'; \
	  echo "$$UNDEFINED"; \
	  exit 1; \
	fi
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 \
	  -T userland/user.ld -o $(USER_ELF) $(USER_OBJ)
	@$(READELF) -h $(USER_ELF) | grep -q 'ELF64' || { echo 'User image is not ELF64'; exit 1; }
	@$(NM) -u $(USER_ELF) 2>/dev/null | grep -q . && { echo 'User ELF has unresolved symbols'; exit 1; } || true
	@$(NM) $(USER_ELF) | grep -q ' _start$$' || { echo 'User ELF has no _start'; exit 1; }

user-verify: check-source $(USER_ELF)
	@echo 'Mistix Pascal ring-3 init ELF verification passed.'
	@$(READELF) -h $(USER_ELF) | grep 'Entry point address'

$(USER_BLOB_OBJ): $(USER_ELF) | $(BUILD_DIR)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o user_init_blob.o user/init.elf

$(KERNEL_OBJ): $(PASCAL_SOURCES) $(USER_BLOB_OBJ) | $(BUILD_DIR)
	$(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU$(BUILD_DIR) -FE$(BUILD_DIR) -Fu$(SRC_DIR) $(SRC_DIR)/mistixkernel.pas
	@test -f $(KERNEL_OBJ) || { echo "FPC did not create $(KERNEL_OBJ)"; exit 1; }

$(KERNEL_ELF): $(KERNEL_OBJ) $(USER_BLOB_OBJ) linker.ld
	@set -eu; \
	UNDEFINED="$$($(NM) -u $(KERNEL_OBJ) 2>/dev/null | grep -v '_binary_user_init_elf_' || true)"; \
	if [ -n "$$UNDEFINED" ]; then \
	  echo 'Unexpected unresolved symbols in the freestanding Pascal kernel object:'; \
	  echo "$$UNDEFINED"; \
	  echo 'Only the embedded user ELF boundary symbols may be unresolved before final link.'; \
	  exit 1; \
	fi
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 \
	  -T linker.ld -o $(KERNEL_ELF) $(KERNEL_OBJ) $(USER_BLOB_OBJ)

kernel: check-source $(KERNEL_ELF)

verify: $(KERNEL_ELF) $(USER_ELF)
	@$(READELF) -h $(KERNEL_ELF) | grep -q 'ELF64' || { echo 'Kernel is not ELF64'; exit 1; }
	@$(NM) -u $(KERNEL_ELF) 2>/dev/null | grep -q . && { echo 'Kernel has unresolved symbols'; exit 1; } || true
	@$(NM) $(KERNEL_ELF) | grep -q ' _start$$' || { echo 'Missing kernel _start'; exit 1; }
	@$(NM) $(KERNEL_ELF) | grep -q ' mistix_syscall_dispatch$$' || { echo 'Missing syscall dispatcher'; exit 1; }
	@$(NM) $(KERNEL_ELF) | grep -q ' mistix_syscall_isr$$' || { echo 'Missing ring-3 syscall entry'; exit 1; }
	@$(NM) $(KERNEL_ELF) | grep -q ' _binary_user_init_elf_start$$' || { echo 'Embedded user image is missing'; exit 1; }
	@echo 'Mistix Phase 3 kernel and user ELF verification passed.'
	@$(READELF) -h $(KERNEL_ELF) | grep 'Entry point address'

check-tools:
	@command -v $(FPC) >/dev/null || { echo 'Missing Free Pascal Compiler (fpc)'; exit 1; }
	@command -v $(LD) >/dev/null || { echo 'Missing GNU ld'; exit 1; }
	@command -v $(NM) >/dev/null || { echo 'Missing nm (binutils)'; exit 1; }
	@command -v $(READELF) >/dev/null || { echo 'Missing readelf (binutils)'; exit 1; }
	@command -v $(XORRISO) >/dev/null || { echo 'Missing xorriso'; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo 'Missing qemu-system-x86_64'; exit 1; }
	@test -x "$(LIMINE_DIR)/bin/limine" || { echo 'Set LIMINE_DIR to a built Limine v12 tree'; exit 1; }

iso: verify
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

run-debug: iso
	$(QEMU) -M q35 -m 256M -smp 1 -cdrom $(ISO_IMAGE) -boot d \
	  -serial stdio -no-reboot -no-shutdown -s -S

clean:
	rm -rf $(BUILD_DIR) *.o *.ppu
