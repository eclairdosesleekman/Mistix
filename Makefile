SHELL := /bin/sh

FPC        ?= fpc
LD         ?= ld
NM         ?= nm
READELF    ?= readelf
XORRISO    ?= xorriso
QEMU       ?= qemu-system-x86_64
PYTHON     ?= python3
LIMINE_DIR ?= ../limine
DISK_IMAGE ?=

BUILD_DIR := build
USER_BUILD_DIR := $(BUILD_DIR)/user
ISO_ROOT := $(BUILD_DIR)/iso_root
KERNEL_OBJ := $(BUILD_DIR)/mistixkernel.o
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
INIT_ELF := $(USER_BUILD_DIR)/init.elf
SHELL_ELF := $(USER_BUILD_DIR)/sh.elf
LS_ELF := $(USER_BUILD_DIR)/ls.elf
CAT_ELF := $(USER_BUILD_DIR)/cat.elf
ECHO_ELF := $(USER_BUILD_DIR)/echo.elf
VFSD_ELF := $(USER_BUILD_DIR)/vfsd.elf
BLOCKD_ELF := $(USER_BUILD_DIR)/blockd.elf
DEVD_ELF := $(USER_BUILD_DIR)/devd.elf
NETD_ELF := $(USER_BUILD_DIR)/netd.elf
VFS_BASEFS_IMG := $(BUILD_DIR)/vfs_base.img
VFS_BASE_BLOB_OBJ := $(USER_BUILD_DIR)/vfs_base_blob.o
INITFS_IMG := $(BUILD_DIR)/initfs.img
INITFS_BLOB_OBJ := $(BUILD_DIR)/initfs_blob.o
USER_BLOB_OBJ := $(BUILD_DIR)/user_init_blob.o
ISO_IMAGE := $(BUILD_DIR)/mistix-phase7.iso
SRC_DIR := src/kernel

KERNEL_SOURCES := \
  $(SRC_DIR)/mistixkernel.pas $(SRC_DIR)/mistix_types.inc \
  $(SRC_DIR)/mistix_console.inc $(SRC_DIR)/mistix_cpu.inc \
  $(SRC_DIR)/mistix_memory.inc $(SRC_DIR)/mistix_threads.inc \
  $(SRC_DIR)/mistix_capability.inc $(SRC_DIR)/mistix_ipc.inc \
  $(SRC_DIR)/mistix_notification.inc $(SRC_DIR)/mistix_memory_object.inc \
  $(SRC_DIR)/mistix_address_space.inc $(SRC_DIR)/mistix_cow.inc \
  $(SRC_DIR)/mistix_block.inc $(SRC_DIR)/mistix_ext2.inc \
  $(SRC_DIR)/mistix_mount.inc $(SRC_DIR)/mistix_net.inc \
  $(SRC_DIR)/mistix_vfs.inc $(SRC_DIR)/mistix_process.inc \
  $(SRC_DIR)/mistix_shm.inc $(SRC_DIR)/mistix_syscall.inc $(SRC_DIR)/mistix_timer.inc \
  $(SRC_DIR)/mistix_trap.inc $(SRC_DIR)/mistix_main.inc

COMMON_INC := userland/common/mistixlib.inc

.PHONY: all check-source check-tools user user-verify initfs kernel verify iso run run-debug clean create-disk

all: check-source user-verify kernel verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(USER_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $(USER_BUILD_DIR)

check-source:
	$(PYTHON) tools/check_source.py

$(INIT_ELF): userland/init/mistixinit.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/init/mistixinit.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixinit.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(SHELL_ELF): userland/sh/mistixsh.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/sh/mistixsh.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixsh.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(LS_ELF): userland/bin/mistixls.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixls.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixls.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(CAT_ELF): userland/bin/mistixcat.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixcat.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixcat.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(ECHO_ELF): userland/bin/mistixecho.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixecho.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixecho.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(VFS_BASEFS_IMG): $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) tools/mkinitfs.py docs/motd.txt | $(BUILD_DIR)
	$(PYTHON) tools/mkinitfs.py $@ --file /bin/sh=$(SHELL_ELF) --file /bin/ls=$(LS_ELF) --file /bin/cat=$(CAT_ELF) --file /bin/echo=$(ECHO_ELF) --file /etc/motd=docs/motd.txt

$(VFS_BASE_BLOB_OBJ): $(VFS_BASEFS_IMG) | $(USER_BUILD_DIR)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o user/vfs_base_blob.o vfs_base.img

$(VFSD_ELF): userland/vfs/mistixvfsd.pas $(COMMON_INC) userland/user.ld $(VFS_BASE_BLOB_OBJ) | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/vfs/mistixvfsd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixvfsd.o $(VFS_BASE_BLOB_OBJ)
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(BLOCKD_ELF): userland/block/mistixblockd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/block/mistixblockd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixblockd.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(DEVD_ELF): userland/device/mistixdevd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/device/mistixdevd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixdevd.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(NETD_ELF): userland/network/mistixnetd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU. -FE. -Fi../../userland/common ../../userland/network/mistixnetd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixnetd.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(INITFS_IMG): $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) $(VFSD_ELF) $(BLOCKD_ELF) $(DEVD_ELF) $(NETD_ELF) tools/mkinitfs.py docs/motd.txt | $(BUILD_DIR)
	$(PYTHON) tools/mkinitfs.py $@ --file /bin/sh=$(SHELL_ELF) --file /bin/ls=$(LS_ELF) --file /bin/cat=$(CAT_ELF) --file /bin/echo=$(ECHO_ELF) --file /sbin/vfsd=$(VFSD_ELF) --file /sbin/blockd=$(BLOCKD_ELF) --file /sbin/devd=$(DEVD_ELF) --file /sbin/netd=$(NETD_ELF) --file /etc/motd=docs/motd.txt

$(INITFS_BLOB_OBJ): $(INITFS_IMG) | $(BUILD_DIR)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o initfs_blob.o initfs.img

$(USER_BLOB_OBJ): $(INIT_ELF) | $(BUILD_DIR)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o user_init_blob.o user/init.elf

$(KERNEL_OBJ): $(KERNEL_SOURCES) $(USER_BLOB_OBJ) $(INITFS_BLOB_OBJ) | $(BUILD_DIR)
	$(FPC) -Tlinux -Px86_64 -Cn -O- -g- -FU$(BUILD_DIR) -FE$(BUILD_DIR) -Fu$(SRC_DIR) $(SRC_DIR)/mistixkernel.pas
	@test -f $(KERNEL_OBJ) || { echo "FPC did not create $(KERNEL_OBJ)"; exit 1; }

$(KERNEL_ELF): $(KERNEL_OBJ) $(USER_BLOB_OBJ) $(INITFS_BLOB_OBJ) linker.ld
	@set -eu; UNDEFINED="$$($(NM) -u $(KERNEL_OBJ) 2>/dev/null | grep -v '_binary_user_init_elf_' | grep -v '_binary_initfs_img_' || true)"; \
	if [ -n "$$UNDEFINED" ]; then echo 'Unexpected unresolved kernel symbols:'; echo "$$UNDEFINED"; exit 1; fi
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T linker.ld -o $@ $(KERNEL_OBJ) $(USER_BLOB_OBJ) $(INITFS_BLOB_OBJ)

user: user-verify
user-verify: check-source $(INIT_ELF) $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) $(VFSD_ELF) $(BLOCKD_ELF) $(DEVD_ELF) $(NETD_ELF) $(INITFS_IMG)
	@echo 'Mistix Phase 7 Pascal services, shared-memory ABI and InitFS images verified.'

initfs: $(INITFS_IMG)
kernel: check-source $(KERNEL_ELF)
verify: $(KERNEL_ELF) $(INIT_ELF) $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) $(VFSD_ELF) $(BLOCKD_ELF) $(DEVD_ELF) $(NETD_ELF)
	@$(READELF) -h $(KERNEL_ELF) | grep -q ELF64
	@if $(NM) -u $(KERNEL_ELF) 2>/dev/null | grep -q .; then echo 'Kernel has unresolved symbols'; $(NM) -u $(KERNEL_ELF); exit 1; fi
	@$(NM) $(KERNEL_ELF) | grep -q ' mistix_trap_dispatch$$'
	@$(NM) $(KERNEL_ELF) | grep -q ' _binary_user_init_elf_start$$'
	@$(NM) $(KERNEL_ELF) | grep -q ' _binary_initfs_img_start$$'
	@echo 'Mistix Phase 7 ELF, shared-memory ABI, VFS, block, device and network services verified.'

check-tools:
	@command -v $(FPC) >/dev/null || { echo 'Missing Free Pascal Compiler (fpc)'; exit 1; }
	@command -v $(LD) >/dev/null || { echo 'Missing GNU ld'; exit 1; }
	@command -v $(NM) >/dev/null || { echo 'Missing nm'; exit 1; }
	@command -v $(READELF) >/dev/null || { echo 'Missing readelf'; exit 1; }
	@command -v $(XORRISO) >/dev/null || { echo 'Missing xorriso'; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo 'Missing qemu-system-x86_64'; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo 'Missing python3'; exit 1; }
	@test -x "$(LIMINE_DIR)/bin/limine" || { echo 'Set LIMINE_DIR to a built Limine v12 tree'; exit 1; }

iso: verify
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
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label $(ISO_ROOT) -o $(ISO_IMAGE)
	$(LIMINE_DIR)/bin/limine bios-install $(ISO_IMAGE)
	@echo 'Created $(ISO_IMAGE)'

run: iso
	@if [ -n "$(DISK_IMAGE)" ]; then \
	  $(QEMU) -M pc -m 256M -smp 1 -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMAGE),format=raw,if=ide -boot d -serial stdio -no-reboot -no-shutdown; \
	else \
	  $(QEMU) -M q35 -m 256M -smp 1 -cdrom $(ISO_IMAGE) -boot d -serial stdio -no-reboot -no-shutdown; \
	fi

run-debug: iso
	$(QEMU) -M q35 -m 256M -smp 1 -cdrom $(ISO_IMAGE) -boot d -serial stdio -no-reboot -no-shutdown -s -S

clean:
	rm -rf $(BUILD_DIR) *.o *.ppu
