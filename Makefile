AS := nasm
CC := gcc
LD := ld
QEMU := qemu-system-x86_64
GRUB_MKRESCUE := grub-mkrescue

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso
KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/os.iso

ASFLAGS := -f elf64
CFLAGS := -ffreestanding -fno-stack-protector -fno-pie -no-pie -m64 \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large \
          -Wall -Wextra -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000 -T src/linker.ld

.PHONY: all clean run iso

all: $(ISO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: src/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o src/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o

iso: $(ISO)

$(ISO): $(KERNEL) grub/grub.cfg
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

clean:
	rm -rf $(BUILD_DIR)
