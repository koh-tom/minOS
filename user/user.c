#include "user.h"

extern char __stack_top[];

int syscall(int sysno, int arg0, int arg1, int arg2) {
  register int a0 __asm__("a0") = arg0;
  register int a1 __asm__("a1") = arg1;
  register int a2 __asm__("a2") = arg2;
  register int a3 __asm__("a3") = sysno;

  __asm__ __volatile__("ecall"
                       : "=r"(a0)
                       : "r"(a0), "r"(a1), "r"(a2), "r"(a3)
                       : "memory");
  return a0;
}

__attribute__((noreturn)) void exit(void) {
  syscall(SYS_EXIT, 0, 0, 0);
  for (;;) {
  }
}

void putchar(char ch) { syscall(SYS_PUTCHAR, ch, 0, 0); }

int getchar(void) { return syscall(SYS_GETCHAR, 0, 0, 0); }

int readfile(const char *filename, char *buf, int len) {
  return syscall(SYS_READFILE, (int)filename, (int)buf, len);
}

int writefile(const char *filename, const char *buf, int len) {
  return syscall(SYS_WRITEFILE, (int)filename, (int)buf, len);
}

int ls(void) { return syscall(SYS_LS, 0, 0, 0); }
int ps(void) { return syscall(SYS_PS, 0, 0, 0); }
int sbrk(int incr) { return syscall(SYS_SBRK, incr, 0, 0); }

// ユーザーランド用 malloc/free
struct header {
  struct header *next;
  size_t size;
};

static struct header free_list;

void *malloc(size_t size) {
  if (free_list.next == NULL && free_list.size == 0) {
    // 簡易初期化
    free_list.next = NULL;
    free_list.size = 0;
  }

  struct header *curr = free_list.next;
  struct header *prev = &free_list;
  size_t total_size = align_up(size + sizeof(struct header), 8);

  while (curr) {
    if (curr->size >= total_size) {
      if (curr->size >= total_size + sizeof(struct header) + 8) {
        struct header *new_node =
            (struct header *)((uintptr_t)curr + total_size);
        new_node->next = curr->next;
        new_node->size = curr->size - total_size;
        curr->next = new_node;
        curr->size = total_size;
      }
      prev->next = curr->next;
      return (void *)(curr + 1);
    }
    prev = curr;
    curr = curr->next;
  }

  // 空きがない場合、sbrkで拡張
  // 効率化のため、少し多めに拡張する (例えば4KB単位)
  int page_size = 4096; // 簡易的
  int alloc_size =
      align_up(total_size > page_size ? total_size : page_size, page_size);

  struct header *new_block = (struct header *)sbrk(alloc_size);
  if ((int)new_block == -1) // sbrk失敗
    return NULL;

  // 確保した領域を初期化
  new_block->size = alloc_size;
  new_block->next = NULL;

  // mallocの処理を再帰的に呼ぶ代わりに、ここで分割ロジックを回すか
  // 単純にfreeして再利用させる
  free((void *)(new_block + 1));

  // 再試行
  return malloc(size);
}

void free(void *ptr) {
  if (!ptr)
    return;

  struct header *block = (struct header *)ptr - 1;
  struct header *curr = free_list.next;
  struct header *prev = &free_list;

  while (curr && curr < block) {
    prev = curr;
    curr = curr->next;
  }

  block->next = curr;
  prev->next = block;

  if ((uintptr_t)block + block->size == (uintptr_t)block->next) {
    block->size += block->next->size;
    block->next = block->next->next;
  }

  if ((uintptr_t)prev + prev->size == (uintptr_t)block) {
    prev->size += block->size;
    prev->next = block->next;
  }
}

__attribute__((section(".text.start"))) __attribute__((naked)) void
start(void) {
  __asm__ __volatile__("mv sp, %[stack_top]\n"
                       "call main\n"
                       "call exit\n"
                       :
                       : [stack_top] "r"(__stack_top));
}