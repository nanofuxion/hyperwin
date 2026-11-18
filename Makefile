CLANG ?= /opt/homebrew/opt/llvm/bin/clang
LLD ?= /opt/homebrew/opt/lld/bin/lld-link
DLLTOOL ?= /opt/homebrew/opt/llvm/bin/llvm-dlltool
HOSTCC ?= cc

BUILD := build
CFLAGS := -O2 -Wall -Wextra -Iinclude -Icore
EFI_CFLAGS := --target=aarch64-pc-windows-msvc -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -fno-builtin -O2 -Iinclude -Icore -DHYPERWIN_FREESTANDING
DLL_CFLAGS := --target=aarch64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -O2 -Iinclude -Icore -Iguest/bootstrap -DHYPERWIN_NO_WINDOWS -DHYPERWIN_FREESTANDING

LD_LLD ?= /opt/homebrew/opt/lld/bin/ld.lld
OBJCOPY ?= /opt/homebrew/opt/llvm/bin/llvm-objcopy
IASL ?= /opt/homebrew/bin/iasl
PAYLOAD_CFLAGS := --target=aarch64-unknown-elf -ffreestanding -fno-stack-protector -fno-builtin -fPIC -mno-outline-atomics -mno-red-zone -O2 -Iinclude -Icore -DHYPERWIN_FREESTANDING

.PHONY: all test efi dll esp payload probe iso bridge
all: test efi dll esp

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/esp/EFI/BOOT $(BUILD)/dist $(BUILD)/obj

$(BUILD)/hwbridge: host/hwbridge.c core/ring.c core/tpm2.c core/sha256.c include/hw_abi.h | $(BUILD)
	$(HOSTCC) $(CFLAGS) -o $@ host/hwbridge.c core/ring.c core/tpm2.c core/sha256.c core/hostfile.c

$(BUILD)/bcd_testsign: core/bcd_testsign.c core/bcd_testsign.h | $(BUILD)
	$(HOSTCC) $(CFLAGS) -DHW_BCD_TEST -o $@ core/bcd_testsign.c

test: $(BUILD)/hwbridge $(BUILD)/bcd_testsign
	$(BUILD)/hwbridge test
	$(BUILD)/bcd_testsign $(BUILD)/originals/BCD-original

$(BUILD)/obj/%.o: %.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/esp/EFI/BOOT/BOOTAA64.EFI: guest/efi/probe.c core/ring.c core/tpm2.c core/sha256.c include/hw_abi.h core/ring.h | $(BUILD)
	$(CLANG) $(EFI_CFLAGS) -c guest/efi/probe.c -o $(BUILD)/obj/probe.o
	$(CLANG) $(EFI_CFLAGS) -c core/ring.c -o $(BUILD)/obj/ring-efi.o
	$(CLANG) $(EFI_CFLAGS) -c core/tpm2.c -o $(BUILD)/obj/tpm-efi.o
	$(CLANG) $(EFI_CFLAGS) -c core/sha256.c -o $(BUILD)/obj/sha-efi.o
	$(LLD) -subsystem:efi_application -entry:EfiMain -out:$@ $(BUILD)/obj/probe.o $(BUILD)/obj/ring-efi.o $(BUILD)/obj/tpm-efi.o $(BUILD)/obj/sha-efi.o

efi: $(BUILD)/esp/EFI/BOOT/BOOTAA64.EFI

$(BUILD)/kernel32.lib: guest/bootstrap/kernel32.def | $(BUILD)
	$(DLLTOOL) -m arm64 -d $< -l $@

$(BUILD)/obj/dll/%.o: guest/bootstrap/%.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(DLL_CFLAGS) -c $< -o $@

$(BUILD)/obj/dll/ring.o: core/ring.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(DLL_CFLAGS) -c $< -o $@

$(BUILD)/obj/dll/tpm.o: core/tpm2.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(DLL_CFLAGS) -c $< -o $@

$(BUILD)/obj/dll/sha.o: core/sha256.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(DLL_CFLAGS) -c $< -o $@

$(BUILD)/obj/dll/icd.o: guest/icd/vulkan_gfxstream.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(DLL_CFLAGS) -Iguest/bootstrap -c $< -o $@

DLL_LINK = $(LLD) -dll -machine:arm64 -entry:DllMain -subsystem:windows -nodefaultlib

$(BUILD)/dist/d3d11.dll: $(BUILD)/obj/dll/d3d11.o $(BUILD)/obj/dll/present.o $(BUILD)/obj/dll/map.o $(BUILD)/obj/dll/ring.o $(BUILD)/obj/dll/tpm.o $(BUILD)/obj/dll/sha.o $(BUILD)/kernel32.lib
	$(DLL_LINK) -out:$@ $(BUILD)/obj/dll/d3d11.o $(BUILD)/obj/dll/present.o $(BUILD)/obj/dll/map.o $(BUILD)/obj/dll/ring.o $(BUILD)/obj/dll/tpm.o $(BUILD)/obj/dll/sha.o $(BUILD)/kernel32.lib

$(BUILD)/dist/dxgi.dll: $(BUILD)/obj/dll/dxgi.o
	$(DLL_LINK) -out:$@ $(BUILD)/obj/dll/dxgi.o

$(BUILD)/dist/vulkan_gfxstream.dll: $(BUILD)/obj/dll/icd.o
	$(DLL_LINK) -out:$@ $(BUILD)/obj/dll/icd.o

dll: $(BUILD)/dist/d3d11.dll $(BUILD)/dist/dxgi.dll $(BUILD)/dist/vulkan_gfxstream.dll

esp: $(BUILD)/esp/EFI/BOOT/BOOTAA64.EFI
	cp tools/efi/startup.nsh $(BUILD)/esp/startup.nsh
	python3 tools/mkfat.py $(BUILD)/esp $(BUILD)/esp.img

$(BUILD)/display-bridge/PROBE.EFI: $(BUILD)/esp/EFI/BOOT/BOOTAA64.EFI | $(BUILD)
	mkdir -p $(BUILD)/display-bridge
	cp $< $@

$(BUILD)/acpi/dsdt.aml: guest/efi/dsdt.asl | $(BUILD)
	mkdir -p $(BUILD)/acpi
	$(IASL) -p $(BUILD)/acpi/dsdt -tc $<

$(BUILD)/acpi/pci_ssdt.aml: guest/efi/pci_ssdt.asl | $(BUILD)
	mkdir -p $(BUILD)/acpi
	$(IASL) -p $(BUILD)/acpi/pci_ssdt -tc $<

$(BUILD)/acpi/acpi_tables.h: $(BUILD)/acpi/dsdt.aml $(BUILD)/acpi/pci_ssdt.aml tools/efi/embed_aml.py
	python3 tools/efi/embed_aml.py $@ HyperwinDsdt=$(BUILD)/acpi/dsdt.aml HyperwinPciSsdt=$(BUILD)/acpi/pci_ssdt.aml

$(BUILD)/display-bridge/gop_driver.obj: guest/efi/gop_driver.c include/hw_abi.h | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/display-bridge/acpi.obj: guest/efi/acpi.c include/hw_abi.h $(BUILD)/acpi/acpi_tables.h | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(EFI_CFLAGS) -I$(BUILD)/acpi -c $< -o $@

$(BUILD)/display-bridge/vec.o: guest/efi/vecstubs.S | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) --target=aarch64-unknown-elf -c $< -o $@

$(BUILD)/display-bridge/vec.bin: $(BUILD)/display-bridge/vec.o
	$(OBJCOPY) -O binary --only-section=.text $< $@

$(BUILD)/display-bridge/vecstubs.h: $(BUILD)/display-bridge/vec.bin tools/efi/embed_aml.py
	python3 tools/efi/embed_aml.py $@ HyperwinVecStubs=$<

$(BUILD)/display-bridge/trace.obj: guest/efi/trace.c include/hw_abi.h | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(EFI_CFLAGS) -I$(BUILD)/display-bridge -c $< -o $@

$(BUILD)/display-bridge/sample.obj: guest/efi/sample.S | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) --target=aarch64-pc-windows-msvc -c $< -o $@

$(BUILD)/display-bridge/GOP.EFI: $(BUILD)/display-bridge/gop_driver.obj $(BUILD)/display-bridge/acpi.obj $(BUILD)/display-bridge/trace.obj
	$(LLD) -subsystem:efi_boot_service_driver -entry:GopDriverEntry -out:$@ $^

$(BUILD)/display-bridge/gop_check.obj: guest/efi/gop_check.c include/hw_abi.h | $(BUILD)
	mkdir -p $(dir $@)
	$(CLANG) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/display-bridge/CHECK.EFI: $(BUILD)/display-bridge/gop_check.obj
	$(LLD) -subsystem:efi_application -entry:GopCheckEntry -out:$@ $<

$(BUILD)/display-bridge.fat: $(BUILD)/display-bridge/PROBE.EFI $(BUILD)/display-bridge/GOP.EFI $(BUILD)/display-bridge/CHECK.EFI tools/mkfat12.py | $(BUILD)
	python3 tools/mkfat12.py $@ --file $(BUILD)/display-bridge/PROBE.EFI PROBE.EFI --file $(BUILD)/display-bridge/GOP.EFI GOP.EFI --file $(BUILD)/display-bridge/CHECK.EFI CHECK.EFI

$(BUILD)/display-bridge.img: $(BUILD)/display-bridge.fat tools/mkgptfat.py
	python3 tools/mkgptfat.py $< $@

app/src/main/assets/display-bridge.img: $(BUILD)/display-bridge.img
	cp $< $@

bridge: app/src/main/assets/display-bridge.img

$(BUILD)/hyperwin.img: guest/payload/head.S guest/payload/main.c guest/payload/payload.ld core/ring.c core/tpm2.c core/sha256.c | $(BUILD)
	$(CLANG) $(PAYLOAD_CFLAGS) -c guest/payload/head.S -o $(BUILD)/obj/head.o
	$(CLANG) $(PAYLOAD_CFLAGS) -c guest/payload/main.c -o $(BUILD)/obj/payload.o
	$(CLANG) $(PAYLOAD_CFLAGS) -c core/ring.c -o $(BUILD)/obj/ring-payload.o
	$(CLANG) $(PAYLOAD_CFLAGS) -c core/tpm2.c -o $(BUILD)/obj/tpm-payload.o
	$(CLANG) $(PAYLOAD_CFLAGS) -c core/sha256.c -o $(BUILD)/obj/sha-payload.o
	$(LD_LLD) -T guest/payload/payload.ld -o $(BUILD)/payload.elf $(BUILD)/obj/head.o $(BUILD)/obj/payload.o $(BUILD)/obj/ring-payload.o $(BUILD)/obj/tpm-payload.o $(BUILD)/obj/sha-payload.o
	$(OBJCOPY) -O binary $(BUILD)/payload.elf $@

payload: $(BUILD)/hyperwin.img

probe: test payload
	tools/qemu/run-probe.sh

iso: esp dll
	tools/qemu/run-iso.sh
