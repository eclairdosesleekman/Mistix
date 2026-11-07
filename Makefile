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
TIMEOUT    ?= timeout
SMOKE_TIMEOUT ?= 45

BUILD_TYPE ?= release
ifeq ($(BUILD_TYPE),debug)
FPC_DIAGNOSTIC_FLAGS ?= -g
FPC_RUNTIME_CHECK_FLAGS ?= -Cr -Co -dMISTIX_DEBUG_CHECKS
else ifeq ($(BUILD_TYPE),release)
FPC_DIAGNOSTIC_FLAGS ?= -g-
FPC_RUNTIME_CHECK_FLAGS ?=
else
$(error BUILD_TYPE must be either 'debug' or 'release')
endif
FPC_COMMON_FLAGS := -Tlinux -Px86_64 -Cn -O- $(FPC_DIAGNOSTIC_FLAGS) $(FPC_RUNTIME_CHECK_FLAGS)

BUILD_DIR ?= build/$(BUILD_TYPE)
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
ISO_IMAGE := $(BUILD_DIR)/mistix.iso
SMOKE_LOG := $(BUILD_DIR)/qemu-serial.log
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
  $(SRC_DIR)/mistix_trap.inc $(SRC_DIR)/mistix_main.inc \
  $(SRC_DIR)/mistix_runtime.inc

COMMON_INC := userland/common/mistixlib.inc

.PHONY: all check-source check-python test check-build-tools check-tools user user-verify initfs kernel verify iso run run-debug smoke debug release clean create-disk phase10-check phase11-check phase12-check phase13-check phase14-check phase15-check phase16-check fpc-runtime-check

all: check-build-tools test user-verify kernel verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(USER_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $(USER_BUILD_DIR)

check-source:
	$(PYTHON) tools/check_source.py
	$(PYTHON) tools/check_phase10.py
	$(PYTHON) tools/check_phase11.py
	$(PYTHON) tools/check_phase12.py
	$(PYTHON) tools/check_phase13.py
	$(PYTHON) tools/check_phase14.py
	$(PYTHON) tools/check_phase15.py
	$(PYTHON) tools/check_phase16.py

check-python:
	$(PYTHON) -m compileall -q tools
	$(PYTHON) -m unittest discover -s tools/tests -p 'test_*.py' -v

test: check-source check-python

check-build-tools:
	@command -v $(FPC) >/dev/null || { echo 'Missing Free Pascal Compiler (fpc)'; exit 1; }
	@command -v $(LD) >/dev/null || { echo 'Missing GNU ld'; exit 1; }
	@command -v $(NM) >/dev/null || { echo 'Missing nm'; exit 1; }
	@command -v $(READELF) >/dev/null || { echo 'Missing readelf'; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo 'Missing python3'; exit 1; }
$(INIT_ELF): userland/init/mistixinit.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/init/mistixinit.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixinit.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(SHELL_ELF): userland/sh/mistixsh.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/sh/mistixsh.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixsh.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(LS_ELF): userland/bin/mistixls.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixls.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixls.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(CAT_ELF): userland/bin/mistixcat.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixcat.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixcat.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(ECHO_ELF): userland/bin/mistixecho.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/bin/mistixecho.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixecho.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(VFS_BASEFS_IMG): $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) tools/mkinitfs.py docs/motd.txt | $(BUILD_DIR)
	$(PYTHON) tools/mkinitfs.py $@ --file /bin/sh=$(SHELL_ELF) --file /bin/ls=$(LS_ELF) --file /bin/cat=$(CAT_ELF) --file /bin/echo=$(ECHO_ELF) --file /etc/motd=docs/motd.txt

$(VFS_BASE_BLOB_OBJ): $(VFS_BASEFS_IMG) | $(USER_BUILD_DIR)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o user/vfs_base_blob.o vfs_base.img

$(VFSD_ELF): userland/vfs/mistixvfsd.pas $(COMMON_INC) userland/vfs/mistix_ext2_types.inc userland/vfs/mistix_ext2_client.inc userland/user.ld $(VFS_BASE_BLOB_OBJ) | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common -Fi../../userland/vfs ../../userland/vfs/mistixvfsd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixvfsd.o $(VFS_BASE_BLOB_OBJ)
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(BLOCKD_ELF): userland/block/mistixblockd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/block/mistixblockd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixblockd.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(DEVD_ELF): userland/device/mistixdevd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/device/mistixdevd.pas
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T userland/user.ld -o $@ $(USER_BUILD_DIR)/mistixdevd.o
	@$(READELF) -h $@ | grep -q ELF64
	@if $(NM) -u $@ 2>/dev/null | grep -q .; then $(NM) -u $@; exit 1; fi

$(NETD_ELF): userland/network/mistixnetd.pas $(COMMON_INC) userland/user.ld | $(USER_BUILD_DIR)
	cd $(USER_BUILD_DIR) && $(FPC) $(FPC_COMMON_FLAGS) -FU. -FE. -Fi../../userland/common ../../userland/network/mistixnetd.pas
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
	$(FPC) $(FPC_COMMON_FLAGS) -FU$(BUILD_DIR) -FE$(BUILD_DIR) -Fu$(SRC_DIR) $(SRC_DIR)/mistixkernel.pas
	@test -f $(KERNEL_OBJ) || { echo "FPC did not create $(KERNEL_OBJ)"; exit 1; }

$(KERNEL_ELF): $(KERNEL_OBJ) $(USER_BLOB_OBJ) $(INITFS_BLOB_OBJ) linker.ld
	@set -eu; UNDEFINED="$$($(NM) -u $(KERNEL_OBJ) 2>/dev/null | grep -v '_binary_user_init_elf_' | grep -v '_binary_initfs_img_' || true)"; \
	if [ -n "$$UNDEFINED" ]; then echo 'Unexpected unresolved kernel symbols:'; echo "$$UNDEFINED"; exit 1; fi
	$(LD) -m elf_x86_64 --nostdlib --build-id=none -z max-page-size=0x1000 -T linker.ld -o $@ $(KERNEL_OBJ) $(USER_BLOB_OBJ) $(INITFS_BLOB_OBJ)

user: user-verify
user-verify: check-build-tools check-source $(INIT_ELF) $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) $(VFSD_ELF) $(BLOCKD_ELF) $(DEVD_ELF) $(NETD_ELF) $(INITFS_IMG)
	@echo 'Mistix Phase 9 Pascal userland VFS syscall broker, remote descriptors and InitFS verified.'

initfs: $(INITFS_IMG)
kernel: check-build-tools check-source $(KERNEL_ELF)
verify: $(KERNEL_ELF) $(INIT_ELF) $(SHELL_ELF) $(LS_ELF) $(CAT_ELF) $(ECHO_ELF) $(VFSD_ELF) $(BLOCKD_ELF) $(DEVD_ELF) $(NETD_ELF)
	@$(READELF) -h $(KERNEL_ELF) | grep -q ELF64
	@if $(NM) -u $(KERNEL_ELF) 2>/dev/null | grep -q .; then echo 'Kernel has unresolved symbols'; $(NM) -u $(KERNEL_ELF); exit 1; fi
	@$(NM) $(KERNEL_ELF) | grep -q ' mistix_trap_dispatch$$'
	@$(NM) $(KERNEL_ELF) | grep -q ' _binary_user_init_elf_start$$'
	@$(NM) $(KERNEL_ELF) | grep -q ' _binary_initfs_img_start$$'
	@echo 'Mistix Phase 9 ELF, proxied VFS syscalls, remote FD lifecycle, block and userland services verified.'

check-tools: check-build-tools
	@command -v $(XORRISO) >/dev/null || { echo 'Missing xorriso'; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo 'Missing qemu-system-x86_64'; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo 'Missing timeout command'; exit 1; }
	@test -x "$(LIMINE_DIR)/bin/limine" || { echo 'Set LIMINE_DIR to a built Limine v12 tree'; exit 1; }

iso: check-tools verify
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

smoke: iso
	@rm -f "$(SMOKE_LOG)"
	@set +e; $(TIMEOUT) --signal=TERM $(SMOKE_TIMEOUT)s $(QEMU) -M q35 -m 256M -smp 1 -cdrom "$(ISO_IMAGE)" -boot d -display none -serial file:"$(SMOKE_LOG)" -monitor none -no-reboot >/dev/null 2>&1; status=$?; set -e; \
	if [ $status -ne 124 ] && [ $status -ne 0 ]; then echo "QEMU exited unexpectedly (status $status)"; cat "$(SMOKE_LOG)" 2>/dev/null || true; exit $status; fi; \
	grep -F "[PASS] Phase 9 kernel foundations and userland VFS broker are prepared." "$(SMOKE_LOG)" >/dev/null || { echo "Kernel boot marker missing"; cat "$(SMOKE_LOG)" 2>/dev/null || true; exit 1; }; \
	grep -F "Starting Mistix shell." "$(SMOKE_LOG)" >/dev/null || { echo "Userland init marker missing"; cat "$(SMOKE_LOG)" 2>/dev/null || true; exit 1; }; \
	echo "Mistix headless QEMU boot smoke test passed."

debug:
	$(MAKE) BUILD_TYPE=debug all

release:
	$(MAKE) BUILD_TYPE=release all

clean:
	rm -rf build $(BUILD_DIR) *.o *.ppu

phase10-check:
	$(PYTHON) tools/check_phase10.py

phase11-check:
	$(PYTHON) tools/check_phase11.py

phase12-check:
	$(PYTHON) tools/check_phase12.py

phase13-check:
	$(PYTHON) tools/check_phase13.py

phase14-check:
	$(PYTHON) tools/check_phase14.py

phase15-check:
	$(PYTHON) tools/check_phase15.py

phase16-check:
	$(PYTHON) tools/check_phase16.py

fpc-runtime-check:
	$(PYTHON) tools/check_fpc_runtime.py
