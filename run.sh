#!/bin/bash
set -xue

# ツールチェーンのパス
QEMU=qemu-system-riscv32
CC=clang
OBJCOPY=llvm-objcopy

# コンパイルオプション
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf \
    -fno-stack-protector -ffreestanding -nostdlib"

# シェルをビルド
$CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
$OBJCOPY -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

# カーネルをビルド
$CC $CFLAGS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c font.c shell.bin.o

# ディスクをビルド
(cd disk && tar cf ../disk.tar --format=ustar *.txt)

# QEMUを起動
$QEMU -machine virt -bios default -serial mon:stdio --no-reboot \
    -d unimp,guest_errors,int,cpu_reset -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -device virtio-gpu-device,bus=virtio-mmio-bus.1 \
    -device virtio-keyboard-device,bus=virtio-mmio-bus.2 \
    -device virtio-mouse-device,bus=virtio-mmio-bus.3 \
    -kernel kernel.elf