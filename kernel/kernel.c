#include "kernel.h"
#include "common.h"

// ブートコード (カーネルエントリポイント)
__attribute__((section(".text.boot"))) __attribute__((naked)) void boot(void) {
  __asm__ __volatile__("mv sp, %[stack_top]\n"
                       "j kernel_main\n"
                       :
                       : [stack_top] "r"(__stack_top));
}

void kernel_main(void) {
  // BSS領域をゼロ初期化
  memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

  // コンソールテスト
  const char *s = "\n\nHello World!\n";
  for (int i = 0; s[i] != '\0'; i++) {
    putchar(s[i]);
  }
  printf("\n\nHello %s (printf)\n", "World!");
  printf("1 + 2 = %d, %x\n", 1 + 2, 0x1234abcd);

  // メモリ割り当てテスト
  paddr_t paddr0 = alloc_pages(2);
  paddr_t paddr1 = alloc_pages(1);
  printf("alloc_pages test: paddr0=%x\n", paddr0);
  printf("alloc_pages test: paddr1=%x\n", paddr1);

  // ヒープ初期化
  void heap_init(void);
  heap_init();

  // kmallocテスト
  void *ptr1 = kmalloc(100);
  void *ptr2 = kmalloc(200);
  printf("kmalloc test: ptr1=%x, ptr2=%x\n", (uint32_t)ptr1, (uint32_t)ptr2);
  kfree(ptr1);
  kfree(ptr2);

  // 例外設定
  WRITE_CSR(stvec, (uint32_t)kernel_entry);

  // ハードウェア初期化
  plic_init();
  virtio_blk_init();
  virtio_gpu_init();
  virtio_input_init();
  fs_init();

  // ディスクテスト
  char buf[SECTOR_SIZE];
  read_write_disk(buf, 0, false);
  printf("first sector: %s\n", buf);

  strcpy(buf, "hello from kernel!!!\n");
  read_write_disk(buf, 0, true);

  // プロセス初期化
  idle_proc = create_process(NULL, 0);
  idle_proc->pid = 0;
  current_proc = idle_proc;

  create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_size);

  yield();
  PANIC("switched to idle process");

  while (1) {
    __asm__ __volatile__("wfi");
  }
}
