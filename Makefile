QEMU = qemu-system-riscv32
CC = clang
OBJCOPY = llvm-objcopy

CFLAGS = -std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf \
         -fno-stack-protector -ffreestanding -nostdlib \
         -Icommon -Ikernel -Iuser

# Sources
KERNEL_SRCS = kernel/kernel.c kernel/font.c common/common.c \
              kernel/alloc.c kernel/proc.c kernel/trap.c kernel/plic.c \
              kernel/virtio.c kernel/virtio_blk.c kernel/virtio_gpu.c \
              kernel/virtio_input.c kernel/fs.c kernel/console.c
USER_SRCS = user/shell.c user/user.c common/common.c

# Intermediate files
SHELL_ELF = shell.elf
SHELL_BIN = shell.bin
SHELL_OBJ = shell.bin.o
KERNEL_ELF = kernel.elf
DISK_IMG = disk.tar

.PHONY: all run clean

all: $(KERNEL_ELF) $(DISK_IMG)

# User Land (Shell) Build
$(SHELL_ELF): $(USER_SRCS) user/user.ld
	$(CC) $(CFLAGS) -Wl,-Tuser/user.ld -Wl,-Map=shell.map -o $@ $(USER_SRCS)

$(SHELL_BIN): $(SHELL_ELF)
	$(OBJCOPY) --set-section-flags .bss=alloc,contents -O binary $< $@

$(SHELL_OBJ): $(SHELL_BIN)
	$(OBJCOPY) -Ibinary -Oelf32-littleriscv $< $@

# Kernel Build
$(KERNEL_ELF): $(KERNEL_SRCS) $(SHELL_OBJ) kernel/kernel.ld
	$(CC) $(CFLAGS) -Wl,-Tkernel/kernel.ld -Wl,-Map=kernel.map -o $@ $(KERNEL_SRCS) $(SHELL_OBJ)

# Disk Image
$(DISK_IMG): disk/spurs.txt
	tar cf $@ --format=ustar -C disk .

# Run
run: $(KERNEL_ELF) $(DISK_IMG)
	$(QEMU) -machine virt -bios /usr/lib/riscv32-linux-gnu/opensbi/generic/fw_dynamic.bin \
		-serial mon:stdio --no-reboot \
		-d unimp,guest_errors,int,cpu_reset -D qemu.log \
		-drive id=drive0,file=$(DISK_IMG),format=raw,if=none \
		-device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
		-device virtio-gpu-device,bus=virtio-mmio-bus.1 \
		-device virtio-keyboard-device,bus=virtio-mmio-bus.2 \
		-device virtio-tablet-device,bus=virtio-mmio-bus.3 \
		-kernel $(KERNEL_ELF)

clean:
	rm -f *.elf *.bin *.o *.map *.tar
