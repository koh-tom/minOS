#include "common.h"
#include "kernel.h"

// メモリ管理
paddr_t alloc_pages(uint32_t n) {
  static paddr_t next_paddr = (paddr_t)__free_ram;
  paddr_t paddr = next_paddr;
  next_paddr += n * PAGE_SIZE;

  if (next_paddr > (paddr_t)__free_ram_end)
    PANIC("out of memory");

  memset((void *)paddr, 0, n * PAGE_SIZE);
  return paddr;
}

void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
  if (!is_aligned(vaddr, PAGE_SIZE)) {
    PANIC("unaligned vaddr %x", vaddr);
  }

  if (!is_aligned(paddr, PAGE_SIZE)) {
    PANIC("unaligned paddr %x", paddr);
  }

  uint32_t vpn1 = (vaddr >> 22) & 0x3FF;
  if ((table1[vpn1] & PAGE_V) == 0) {
    uint32_t pt_paddr = alloc_pages(1);
    table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
  }

  uint32_t vpn0 = (vaddr >> 12) & 0x3FF;
  uint32_t *table0 = (uint32_t *)((table1[vpn1] >> 10) * PAGE_SIZE);
  table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | PAGE_V | flags;
}
// 動的メモリ管理（ヒープ）
struct header {
  struct header *next;
  size_t size;
};

struct header free_list; // ダミーノード

void heap_init(void) {
  free_list.next = NULL;
  free_list.size = 0;
}

void *kmalloc(size_t size) {
  struct header *curr = free_list.next;
  struct header *prev = &free_list;
  size_t total_size =
      align_up(size + sizeof(struct header), 8); // 8バイトアライメント

  // 空きリストから十分なサイズのブロックを探す
  while (curr) {
    if (curr->size >= total_size) {
      if (curr->size >= total_size + sizeof(struct header) + 8) {
        // 分割可能
        struct header *new_node =
            (struct header *)((uintptr_t)curr + total_size);
        new_node->next = curr->next;
        new_node->size = curr->size - total_size;
        curr->next = new_node;
        curr->size = total_size;
      }

      // リストから外す
      prev->next = curr->next;
      return (void *)(curr + 1); // ヘッダの直後を返す
    }
    prev = curr;
    curr = curr->next;
  }

  // 空きがない場合、新しいページを割り当てる
  int num_pages = align_up(total_size, PAGE_SIZE) / PAGE_SIZE;
  struct header *new_block = (struct header *)alloc_pages(num_pages);
  size_t block_size = num_pages * PAGE_SIZE;

  if (block_size > total_size + sizeof(struct header) + 8) {
    // 残りを空きリストに追加
    struct header *remainder =
        (struct header *)((uintptr_t)new_block + total_size);
    remainder->size = block_size - total_size;
    kfree((void *)(remainder + 1));
    new_block->size = total_size;
  } else {
    new_block->size = block_size;
  }

  return (void *)(new_block + 1);
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  struct header *block = (struct header *)ptr - 1;
  struct header *curr = free_list.next;
  struct header *prev = &free_list;

  // アドレス順になるように挿入位置を探す
  while (curr && curr < block) {
    prev = curr;
    curr = curr->next;
  }

  // リストに挿入
  block->next = curr;
  prev->next = block;

  // 右隣と結合できるか確認
  if ((uintptr_t)block + block->size == (uintptr_t)block->next) {
    block->size += block->next->size;
    block->next = block->next->next;
  }

  // 左隣と結合できるか確認
  if ((uintptr_t)prev + prev->size == (uintptr_t)block) {
    prev->size += block->size;
    prev->next = block->next;
  }
}
