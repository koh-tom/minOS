#include "kernel.h"
#include "common.h"

// 外部シンボル
extern char __bss[], __bss_end[], __stack_top[];
extern char __free_ram[], __free_ram_end[];
extern char __kernel_base[];
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

// プロトタイプ宣言
void yield(void);
void handle_syscall(struct trap_frame *f);
uint32_t virtio_reg_read32(uint32_t base, unsigned offset);
uint64_t virtio_reg_read64(uint32_t base, unsigned offset);
void virtio_reg_write32(uint32_t base, unsigned offset, uint32_t value);
void virtio_reg_fetch_and_or32(uint32_t base, unsigned offset, uint32_t value);
void fs_flush(void);

// プロセス管理
struct process procs[PROCS_MAX];
struct process *proc_a;
struct process *proc_b;

// スケジューラ用
struct process *current_proc;
struct process *idle_proc;

// VIRTIO用
struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

// ファイルシステム用
struct file files[FILES_MAX];
uint8_t disk[DISK_MAX_SIZE];

uint32_t keyboard_paddr = 0;
struct virtio_virtq *keyboard_vq;
struct virtio_input_event keyboard_event_bufs[VIRTQ_ENTRY_NUM];

uint32_t mouse_paddr = 0;
struct virtio_virtq *mouse_vq;
struct virtio_input_event mouse_event_bufs[VIRTQ_ENTRY_NUM];

struct file *fs_lookup(const char *filename) {
  for (int i = 0; i < FILES_MAX; i++) {
    struct file *file = &files[i];
    if (!strcmp(file->name, filename))
      return file;
  }

  return NULL;
}

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

// OpenSBI呼び出し
struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {
  register long a0 __asm__("a0") = arg0;
  register long a1 __asm__("a1") = arg1;
  register long a2 __asm__("a2") = arg2;
  register long a3 __asm__("a3") = arg3;
  register long a4 __asm__("a4") = arg4;
  register long a5 __asm__("a5") = arg5;
  register long a6 __asm__("a6") = fid;
  register long a7 __asm__("a7") = eid;

  __asm__ volatile("ecall"
                   : "=r"(a0), "=r"(a1)
                   : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                     "r"(a6), "r"(a7)
                   : "memory");
  return (struct sbiret){.error = a0, .value = a1};
}

struct virtio_virtq *virtq_init(uint32_t base, unsigned index) {
  paddr_t virtq_paddr =
      alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
  struct virtio_virtq *virtq = (struct virtio_virtq *)virtq_paddr;
  virtq->queue_index = index;
  virtq->used_index = (volatile uint16_t *)&virtq->used.index;
  virtq->reg_base = base;

  // キュー選択
  virtio_reg_write32(base, VIRTIO_REG_QUEUE_SEL, index);
  // キューサイズ
  virtio_reg_write32(base, VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
  // キューページフレーム番号
  virtio_reg_write32(base, VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
  return virtq;
}

void plic_init(void);
void virtio_input_init(void);

// 外部割り込み用SCause (最上位ビットが1)
#define SCAUSE_INTERRUPT 0x80000000
#define SCAUSE_EXTERNAL_INTERRUPT (SCAUSE_INTERRUPT | 9)

// 1文字出力用関数
void putchar(char ch) { sbi_call(ch, 0, 0, 0, 0, 0, 0, 1); }

// PLICの初期化
void plic_init(void) {
  // すべての割り込みの優先度を1(最低)にする
  for (int i = 1; i <= 32; i++) {
    virtio_reg_write32(PLIC_PRIORITY(i), 0, 1);
  }

  // Hart 0 の S-Mode に対してキーボード、マウスの割り込みを有効化
  virtio_reg_write32(PLIC_SENABLE(0, 0), 0,
                     (1 << VIRTIO_KEYBOARD_IRQ) | (1 << VIRTIO_MOUSE_IRQ));

  // 割り込みのしきい値を0（すべて通す）に設定
  virtio_reg_write32(PLIC_SPRIORITY(0), 0, 0);

  // CPU側の外部割り込み許可 (SIE.SEIE)
  WRITE_CSR(sie, READ_CSR(sie) | SIE_SEIE);

  // 全体的な割り込み有効化 (SSTATUS.SIE)
  WRITE_CSR(sstatus, READ_CSR(sstatus) | SSTATUS_SIE);
}

// キーボードイベントのリングバッファ
struct virtio_input_event keyboard_queue[64];
int keyboard_head = 0;
int keyboard_tail = 0;

void keyboard_push(struct virtio_input_event event) {
  int next = (keyboard_head + 1) % 64;
  if (next != keyboard_tail) {
    keyboard_queue[keyboard_head] = event;
    keyboard_head = next;
  }
}

struct virtio_input_event *keyboard_pop(void) {
  if (keyboard_head == keyboard_tail) {
    return NULL;
  }
  struct virtio_input_event *event = &keyboard_queue[keyboard_tail];
  keyboard_tail = (keyboard_tail + 1) % 64;
  return event;
}

int key2char(uint16_t code) {
  if (code >= 2 && code <= 11)
    return "1234567890"[code - 2];
  if (code >= 16 && code <= 25)
    return "qwertyuiop"[code - 16];
  if (code >= 30 && code <= 38)
    return "asdfghjkl"[code - 30];
  if (code >= 44 && code <= 50)
    return "zxcvbnm"[code - 44];

  if (code == 57)
    return ' ';
  if (code == 28)
    return '\n';
  if (code == 14)
    return '\b';
  return 0;
}

// 1文字入力用関数 (Virtio-Input経由)
// 1文字入力用関数 (Virtio-Input または SBI経由)
long getchar(void) {
  // 1. Virtio-Keyboard の入力をチェック
  struct virtio_input_event *event = keyboard_pop();
  if (event) {
    // Value=1 (Press) または Value=2 (Repeat) のみ処理
    if ((event->value == 1 || event->value == 2) && event->type == 1) {
      int ch = key2char(event->code);
      if (ch)
        return ch;
    }
  }

  // 2. シリアルポート(SBI) の入力をチェック
  struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
  if (ret.error != -1) {
    return ret.error;
  }

  return -1;
}

// マウスイベントのリングバッファ
int mouse_x = 0;
int mouse_y = 0;

uint32_t screen_w = 640;
uint32_t screen_h = 480;
uint32_t *framebuffer;

void draw_cursor(int prev_x, int prev_y, int new_x, int new_y);

// トラップハンドラ
void handle_trap(struct trap_frame *f) {
  uint32_t scause = READ_CSR(scause);
  uint32_t stval = READ_CSR(stval);
  uint32_t user_pc = READ_CSR(sepc);

  if (scause == SCAUSE_ECALL) {
    handle_syscall(f);
    user_pc += 4;
  } else if (scause == SCAUSE_EXTERNAL_INTERRUPT) {
    // 外部割り込み (PLIC経由)
    uint32_t irq = virtio_reg_read32(PLIC_SCLAIM(0), 0);
    // printf("IRQ: %d\n", irq); // デバッグ用

    if (irq == VIRTIO_KEYBOARD_IRQ && keyboard_vq) {
      // キーボード割り込みの処理
      while (keyboard_vq->last_used_index != *keyboard_vq->used_index) {
        struct virtq_used_elem *e =
            &keyboard_vq->used
                 .ring[keyboard_vq->last_used_index % VIRTQ_ENTRY_NUM];
        uint32_t desc_idx = e->id;

        // デバイスから書き込まれたイベントを取得
        struct virtio_input_event *event =
            (struct virtio_input_event *)keyboard_vq->descs[desc_idx].addr;

        if (event->type == 1) {
          keyboard_push(*event);
        }

        // バッファを再利用のためにAvailリングに戻す
        keyboard_vq->avail.ring[keyboard_vq->avail.index % VIRTQ_ENTRY_NUM] =
            desc_idx;
        keyboard_vq->avail.index++;
        keyboard_vq->last_used_index++;
      }
      __sync_synchronize();
      virtio_reg_write32(keyboard_paddr, VIRTIO_REG_QUEUE_NOTIFY, 0);
    } else if (irq == VIRTIO_MOUSE_IRQ && mouse_vq) {
      // マウス割り込みの処理
      static int prev_mouse_x = 0;
      static int prev_mouse_y = 0;
      bool updated = false;

      while (mouse_vq->last_used_index != *mouse_vq->used_index) {
        struct virtq_used_elem *e =
            &mouse_vq->used.ring[mouse_vq->last_used_index % VIRTQ_ENTRY_NUM];
        uint32_t desc_idx = e->id;

        struct virtio_input_event *event =
            (struct virtio_input_event *)mouse_vq->descs[desc_idx].addr;

        // Type 3 は EV_ABS (絶対座標)
        if (event->type == 3) {
          if (event->code == 0) { // ABS_X
            mouse_x = ((uint64_t)event->value * screen_w) / 32768;
            updated = true;
          } else if (event->code == 1) { // ABS_Y
            mouse_y = ((uint64_t)event->value * screen_h) / 32768;
            updated = true;
          }
        }

        // 画面外に出ないように制限
        if (mouse_x < 0)
          mouse_x = 0;
        if (mouse_x >= screen_w)
          mouse_x = screen_w - 1;
        if (mouse_y < 0)
          mouse_y = 0;
        if (mouse_y >= screen_h)
          mouse_y = screen_h - 1;

        // バッファを再利用のためにAvailリングに戻す
        mouse_vq->avail.ring[mouse_vq->avail.index % VIRTQ_ENTRY_NUM] =
            desc_idx;
        mouse_vq->avail.index++;
        mouse_vq->last_used_index++;
      }

      if (updated) {
        draw_cursor(prev_mouse_x, prev_mouse_y, mouse_x, mouse_y);
        prev_mouse_x = mouse_x;
        prev_mouse_y = mouse_y;
      }

      __sync_synchronize();
      virtio_reg_write32(mouse_paddr, VIRTIO_REG_QUEUE_NOTIFY, 0);
    }

    // 完了を通知
    if (irq) {
      virtio_reg_write32(PLIC_SCLAIM(0), 0, irq);
    }
  } else {
    PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval,
          user_pc);
  }
  WRITE_CSR(sepc, user_pc);
}

// 例外発生時のエントリー
__attribute__((naked)) __attribute__((aligned(4))) void kernel_entry(void) {
  __asm__ __volatile__("csrrw sp, sscratch, sp\n"
                       "addi sp, sp, -4 * 31\n"
                       "sw ra,  4 * 0(sp)\n"
                       "sw gp,  4 * 1(sp)\n"
                       "sw tp,  4 * 2(sp)\n"
                       "sw t0,  4 * 3(sp)\n"
                       "sw t1,  4 * 4(sp)\n"
                       "sw t2,  4 * 5(sp)\n"
                       "sw t3,  4 * 6(sp)\n"
                       "sw t4,  4 * 7(sp)\n"
                       "sw t5,  4 * 8(sp)\n"
                       "sw t6,  4 * 9(sp)\n"
                       "sw a0,  4 * 10(sp)\n"
                       "sw a1,  4 * 11(sp)\n"
                       "sw a2,  4 * 12(sp)\n"
                       "sw a3,  4 * 13(sp)\n"
                       "sw a4,  4 * 14(sp)\n"
                       "sw a5,  4 * 15(sp)\n"
                       "sw a6,  4 * 16(sp)\n"
                       "sw a7,  4 * 17(sp)\n"
                       "sw s0,  4 * 18(sp)\n"
                       "sw s1,  4 * 19(sp)\n"
                       "sw s2,  4 * 20(sp)\n"
                       "sw s3,  4 * 21(sp)\n"
                       "sw s4,  4 * 22(sp)\n"
                       "sw s5,  4 * 23(sp)\n"
                       "sw s6,  4 * 24(sp)\n"
                       "sw s7,  4 * 25(sp)\n"
                       "sw s8,  4 * 26(sp)\n"
                       "sw s9,  4 * 27(sp)\n"
                       "sw s10, 4 * 28(sp)\n"
                       "sw s11, 4 * 29(sp)\n"

                       "csrr a0, sscratch\n"
                       "sw a0, 4 * 30(sp)\n"

                       "addi a0, sp, 4 * 31\n"
                       "csrw sscratch, a0\n"

                       "mv a0, sp\n"
                       "call handle_trap\n"

                       "lw ra,  4 * 0(sp)\n"
                       "lw gp,  4 * 1(sp)\n"
                       "lw tp,  4 * 2(sp)\n"
                       "lw t0,  4 * 3(sp)\n"
                       "lw t1,  4 * 4(sp)\n"
                       "lw t2,  4 * 5(sp)\n"
                       "lw t3,  4 * 6(sp)\n"
                       "lw t4,  4 * 7(sp)\n"
                       "lw t5,  4 * 8(sp)\n"
                       "lw t6,  4 * 9(sp)\n"
                       "lw a0,  4 * 10(sp)\n"
                       "lw a1,  4 * 11(sp)\n"
                       "lw a2,  4 * 12(sp)\n"
                       "lw a3,  4 * 13(sp)\n"
                       "lw a4,  4 * 14(sp)\n"
                       "lw a5,  4 * 15(sp)\n"
                       "lw a6,  4 * 16(sp)\n"
                       "lw a7,  4 * 17(sp)\n"
                       "lw s0,  4 * 18(sp)\n"
                       "lw s1,  4 * 19(sp)\n"
                       "lw s2,  4 * 20(sp)\n"
                       "lw s3,  4 * 21(sp)\n"
                       "lw s4,  4 * 22(sp)\n"
                       "lw s5,  4 * 23(sp)\n"
                       "lw s6,  4 * 24(sp)\n"
                       "lw s7,  4 * 25(sp)\n"
                       "lw s8,  4 * 26(sp)\n"
                       "lw s9,  4 * 27(sp)\n"
                       "lw s10, 4 * 28(sp)\n"
                       "lw s11, 4 * 29(sp)\n"
                       "lw sp,  4 * 30(sp)\n"
                       "sret\n");
}

// コンテキストスイッチ
__attribute__((naked)) void switch_context(uint32_t *prev_sp,
                                           uint32_t *next_sp) {
  __asm__ __volatile__("addi sp, sp, -13 * 4\n"
                       "sw ra,  0  * 4(sp)\n"
                       "sw s0,  1  * 4(sp)\n"
                       "sw s1,  2  * 4(sp)\n"
                       "sw s2,  3  * 4(sp)\n"
                       "sw s3,  4  * 4(sp)\n"
                       "sw s4,  5  * 4(sp)\n"
                       "sw s5,  6  * 4(sp)\n"
                       "sw s6,  7  * 4(sp)\n"
                       "sw s7,  8  * 4(sp)\n"
                       "sw s8,  9  * 4(sp)\n"
                       "sw s9,  10 * 4(sp)\n"
                       "sw s10, 11 * 4(sp)\n"
                       "sw s11, 12 * 4(sp)\n"

                       "sw sp, (a0)\n"
                       "lw sp, (a1)\n"

                       "lw ra,  0  * 4(sp)\n"
                       "lw s0,  1  * 4(sp)\n"
                       "lw s1,  2  * 4(sp)\n"
                       "lw s2,  3  * 4(sp)\n"
                       "lw s3,  4  * 4(sp)\n"
                       "lw s4,  5  * 4(sp)\n"
                       "lw s5,  6  * 4(sp)\n"
                       "lw s6,  7  * 4(sp)\n"
                       "lw s7,  8  * 4(sp)\n"
                       "lw s8,  9  * 4(sp)\n"
                       "lw s9,  10 * 4(sp)\n"
                       "lw s10, 11 * 4(sp)\n"
                       "lw s11, 12 * 4(sp)\n"
                       "addi sp, sp, 13 * 4\n"
                       "ret\n");
}

// ページテーブルの設定
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

__attribute__((naked)) void user_entry(void) {
  __asm__ __volatile__(
      "csrw sepc, %[sepc]\n"
      "csrw sstatus, %[sstatus]\n"
      "sret\n"
      :
      : [sepc] "r"(USER_BASE), [sstatus] "r"(SSTATUS_SPIE | SSTATUS_SUM));
}

// プロセス生成
struct process *create_process(const void *image, size_t image_size) {
  struct process *proc = NULL;
  int i;

  for (i = 0; i < PROCS_MAX; i++) {
    if (procs[i].state == PROCS_UNUSED) {
      proc = &procs[i];
      break;
    }
  }
  if (!proc) {
    PANIC("out of processes\n");
  }

  uint32_t *sp = (uint32_t *)&proc->stack[sizeof(proc->stack)];
  *--sp = 0;                    // s11
  *--sp = 0;                    // s10
  *--sp = 0;                    // s9
  *--sp = 0;                    // s8
  *--sp = 0;                    // s7
  *--sp = 0;                    // s6
  *--sp = 0;                    // s5
  *--sp = 0;                    // s4
  *--sp = 0;                    // s3
  *--sp = 0;                    // s2
  *--sp = 0;                    // s1
  *--sp = 0;                    // s0
  *--sp = (uint32_t)user_entry; // ra

  uint32_t *page_table = (uint32_t *)alloc_pages(1);

  for (paddr_t paddr = (paddr_t)__kernel_base; paddr < (paddr_t)__free_ram_end;
       paddr += PAGE_SIZE) {
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);
  }

  // VIRTIOのMMIO領域をマッピング
  for (paddr_t paddr = VIRTIO_BLK_PADDR; paddr < VIRTIO_BLK_PADDR + 0x8000;
       paddr += PAGE_SIZE) {
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W);
  }

  // PLICのMMIO領域をマッピング (0x0c000000 ~ 0x0c0400000, 4MB)
  for (paddr_t paddr = PLIC_BASE; paddr < PLIC_BASE + 0x400000;
       paddr += PAGE_SIZE) {
    if (paddr >= 0x10000000) // 念のため
      break;
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W);
  }

  for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
    paddr_t page = alloc_pages(1);
    size_t remaining = image_size - off;
    size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;

    memcpy((void *)page, image + off, copy_size);
    map_page(page_table, USER_BASE + off, page,
             PAGE_U | PAGE_R | PAGE_W | PAGE_X);
  }

  proc->pid = i + 1;
  proc->state = PROCS_RUNNABLE;
  proc->sp = (uint32_t)sp;
  proc->page_table = page_table;
  return proc;
}

// スケジューラ
void yield(void) {
  struct process *next = idle_proc;
  for (int i = 0; i < PROCS_MAX; i++) {
    struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
    if (proc->state == PROCS_RUNNABLE && proc->pid > 0) {
      next = proc;
      break;
    }
  }
  if (next == current_proc) {
    return;
  }

  __asm__ __volatile__(
      "sfence.vma\n"
      "csrw satp, %[satp]\n"
      "sfence.vma\n"
      "csrw sscratch, %[sscratch]\n"
      :
      : [satp] "r"(SATP_SV32 | ((uint32_t)next->page_table / PAGE_SIZE)),
        [sscratch] "r"((uint32_t)&next->stack[sizeof(next->stack)]));

  struct process *prev = current_proc;
  current_proc = next;
  switch_context(&prev->sp, &next->sp);
}

// ディレイ関数
void delay(void) {
  for (int i = 0; i < 30000000; i++) {
    __asm__ __volatile__("nop");
  }
}

// プロセスAのエントリ
void proc_a_entry(void) {
  printf("start process_a\n");
  while (1) {
    putchar('A');
    // switch_context(&proc_a->sp, &proc_b->sp);
    yield();
    delay();
  }
}

// プロセスBのエントリ
void proc_b_entry(void) {
  printf("start process_b\n");
  while (1) {
    putchar('B');
    // switch_context(&proc_b->sp, &proc_a->sp);
    yield();
    delay();
  }
}

// システムコールハンドラ
void handle_syscall(struct trap_frame *f) {
  switch (f->a3) {
  case SYS_PUTCHAR:
    putchar(f->a0);
    break;
  case SYS_GETCHAR:
    f->a0 = getchar();
    break;
  case SYS_EXIT:
    printf("process %d exited\n", current_proc->pid);
    current_proc->state = PROC_EXITED;
    yield();
    PANIC("unreachable");
    break;
  case SYS_READFILE:
  case SYS_WRITEFILE: {
    const char *filename = (const char *)f->a0;
    char *buf = (char *)f->a1;
    int len = f->a2;
    struct file *file = fs_lookup(filename);
    if (!file) {
      printf("file not found: %s\n", filename);
      f->a0 = -1;
      break;
    }

    if (len > (int)sizeof(file->data))
      len = file->size;

    if (f->a3 == SYS_WRITEFILE) {
      memcpy(file->data, buf, len);
      file->size = len;
      fs_flush();
    } else {
      memcpy(buf, file->data, len);
    }

    f->a0 = len;
    break;
  }
  case SYS_LS: {
    for (int i = 0; i < FILES_MAX; i++) {
      if (files[i].in_use) {
        printf("%s  ", files[i].name);
      }
    }
    printf("\n");
    break;
  }
  default:
    PANIC("unexpected syscall a3=%x\n", f->a3);
  }
}

// VIRTIOのMMIOレジスタの読み書き用関数群
// VIRTIOのMMIOレジスタの読み書き用関数群
uint32_t virtio_reg_read32(uint32_t base, unsigned offset) {
  return *((volatile uint32_t *)(base + offset));
}

uint64_t virtio_reg_read64(uint32_t base, unsigned offset) {
  return *((volatile uint64_t *)(base + offset));
}

void virtio_reg_write32(uint32_t base, unsigned offset, uint32_t value) {
  *((volatile uint32_t *)(base + offset)) = value;
}

void virtio_reg_fetch_and_or32(uint32_t base, unsigned offset, uint32_t value) {
  virtio_reg_write32(base, offset, virtio_reg_read32(base, offset) | value);
}

// デバイスに新しいリクエストがあることを通知
void virtq_kick(struct virtio_virtq *virtq, int desc_index) {
  // printf("virtq_kick: reg_base=%x, queue_index=%d\n", virtq->reg_base,
  // virtq->queue_index);
  virtq->avail.ring[virtq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
  virtq->avail.index++;
  __sync_synchronize();
  virtio_reg_write32(virtq->reg_base, VIRTIO_REG_QUEUE_NOTIFY,
                     virtq->queue_index);
  virtq->last_used_index++;
}

// デバイスがリクエストを処理しているか
bool virtq_is_busy(struct virtio_virtq *virtq) {
  return virtq->last_used_index != *virtq->used_index;
}

// ディスクの読み書き
void read_write_disk(void *buf, unsigned sector, int is_write) {
  if (sector >= blk_capacity / SECTOR_SIZE) {
    printf("virtio: tried to read/write sector %d, but capacity is %d\n",
           sector, blk_capacity / SECTOR_SIZE);
    return;
  }

  blk_req->sector = sector;
  blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;

  if (is_write) {
    memcpy(blk_req->data, buf, SECTOR_SIZE);
  }

  // ディスクリプタの設定
  struct virtio_virtq *virtq = blk_request_vq;
  virtq->descs[0].addr = blk_req_paddr;
  virtq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
  virtq->descs[0].flags = VIRTQ_DESC_F_NEXT;
  virtq->descs[0].next = 1;

  virtq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
  virtq->descs[1].len = SECTOR_SIZE;
  virtq->descs[1].flags =
      VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
  virtq->descs[1].next = 2;

  virtq->descs[2].addr =
      blk_req_paddr + offsetof(struct virtio_blk_req, status);
  virtq->descs[2].len = sizeof(uint8_t);
  virtq->descs[2].flags = VIRTQ_DESC_F_WRITE;

  // 新しいリクエストを通知
  virtq_kick(virtq, 0);

  // 終了まち
  while (virtq_is_busy(virtq)) {
  }

  // 0以外の場合は失敗
  if (blk_req->status != 0) {
    printf("virtio: warn: failed to read/write disk sector=%d status=%d\n",
           sector, blk_req->status);
    return;
  }

  // 読み込みの場合はバッハにコピー
  if (!is_write) {
    memcpy(buf, blk_req->data, SECTOR_SIZE);
  }
}

void virtio_blk_init(void) {
  if (virtio_reg_read32(VIRTIO_BLK_PADDR, VIRTIO_REG_MAGIC) != 0x74726976) {
    PANIC("virtio : invalid magic value\n");
  }
  if (virtio_reg_read32(VIRTIO_BLK_PADDR, VIRTIO_REG_VERSION) != 1)
    PANIC("virtio: invalid version\n");
  if (virtio_reg_read32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_ID) !=
      VIRTIO_DEVICE_BLK)
    PANIC("virtio: invalid device id\n");

  // VIRTIOの初期化
  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS, 0);

  // Acknowledgeステータスビットを立てる
  virtio_reg_fetch_and_or32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_ACK);

  // Driverステータスビットを立てる
  virtio_reg_fetch_and_or32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_DRIVER);

  // ページサイズを設定(4KB)
  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

  // ディスク読み書き用のキューを初期化
  blk_request_vq = virtq_init(VIRTIO_BLK_PADDR, 0);

  // DriverOKステータスビットを立てる
  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                     VIRTIO_STATUS_DRIVER_OK);

  // ディスク容量を取得
  blk_capacity =
      virtio_reg_read64(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_CONFIG + 0) *
      SECTOR_SIZE;
  printf("blk_capacity: %d\n", (int)blk_capacity);

  // デバイス要求用のメモリを確保
  blk_req_paddr =
      alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
  blk_req = (struct virtio_blk_req *)blk_req_paddr;
}

uint32_t virtio_gpu_paddr = 0;
struct virtio_virtq *gpu_control_vq;
struct virtio_gpu_config *gpu_config;

// GPUにリクエストを送信し、応答を待つ
void virtio_gpu_send_req(void *req, int len) {
  struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)req;
  hdr->flags = VIRTIO_GPU_FLAG_FENCE;
  hdr->fence_id = 0;
  hdr->ctx_id = 0;

  // リクエスト用のディスクリプタを設定
  struct virtio_virtq *virtq = gpu_control_vq;
  virtq->descs[0].addr = (uint32_t)req;
  virtq->descs[0].len = len;
  virtq->descs[0].flags = VIRTQ_DESC_F_NEXT;
  virtq->descs[0].next = 1;

  // レスポンス用のディスクリプタを設定
  virtq->descs[1].addr = (uint32_t)req;
  virtq->descs[1].len = sizeof(struct virtio_gpu_ctrl_hdr);
  virtq->descs[1].flags = VIRTQ_DESC_F_WRITE;
  virtq->descs[1].next = 0;

  // キック & Wait
  virtq->avail.ring[virtq->avail.index % VIRTQ_ENTRY_NUM] = 0;
  virtq->avail.index++;
  __sync_synchronize();
  virtio_reg_write32(virtq->reg_base, VIRTIO_REG_QUEUE_NOTIFY,
                     virtq->queue_index);
  virtq->last_used_index++;

  // ポーリング
  while (virtq->last_used_index != *virtq->used_index) {
  }
}

extern uint8_t font_bitmap[256][16];

void draw_char(char c, int x, int y, uint32_t color) {
  for (int dy = 0; dy < 16; dy++) {
    uint8_t row = font_bitmap[(uint8_t)c][dy];
    for (int dx = 0; dx < 8; dx++) {
      if ((row >> (7 - dx)) & 1) {
        if (x + dx < screen_w && y + dy < screen_h) {
          // B8G8R8A8 format, little endian
          framebuffer[(y + dy) * screen_w + (x + dx)] = color;
        }
      }
    }
  }
}

void draw_string(const char *s, int x, int y, uint32_t color) {
  int cx = x;
  int cy = y;
  while (*s) {
    if (*s == '\n') {
      cx = x;
      cy += 16;
    } else {
      draw_char(*s, cx, cy, color);
      cx += 8;
    }
    s++;
  }
}

void virtio_gpu_init(void) {
  printf("Probing for Virtio-GPU...\n");
  uint32_t *paddr = (uint32_t *)VIRTIO_BLK_PADDR;
  // MMIO領域をスキャンしてGPUを探す
  bool found = false;
  for (int i = 0; i < 8; i++) {
    uint32_t magic = paddr[0];
    uint32_t device_id = paddr[2];
    if (magic == 0x74726976 && device_id == VIRTIO_DEVICE_GPU) {
      printf("found virtio-gpu at %x\n", (uint32_t)paddr);
      virtio_gpu_paddr = (uint32_t)paddr;
      found = true;
      break;
    }
    paddr = (uint32_t *)((uint32_t)paddr + 0x1000);
  }

  if (!found)
    PANIC("virtio-gpu not found");

  // デバイスの初期化
  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS, 0);
  virtio_reg_fetch_and_or32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_ACK);
  virtio_reg_fetch_and_or32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_DRIVER);

  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

  // Control Queue (index 0) の初期化
  gpu_control_vq = virtq_init(virtio_gpu_paddr, 0);

  // DriverOK
  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                     VIRTIO_STATUS_DRIVER_OK);

  // フレームバッファの確保
  framebuffer = (uint32_t *)alloc_pages(
      align_up(screen_w * screen_h * 4, PAGE_SIZE) / PAGE_SIZE);
  memset(framebuffer, 0, screen_w * screen_h * 4);

  // 1. 2Dリソース作成
  struct virtio_gpu_resource_create_2d res_create = {0};
  res_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  res_create.resource_id = 1;
  res_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  res_create.width = screen_w;
  res_create.height = screen_h;
  virtio_gpu_send_req(&res_create, sizeof(res_create));

  // 2. バッキングストレージのアタッチ
  struct {
    struct virtio_gpu_resource_attach_backing attach;
    struct virtio_gpu_mem_entry entry;
  } __attribute__((packed)) req_attach = {0};

  req_attach.attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  req_attach.attach.resource_id = 1;
  req_attach.attach.nr_entries = 1;
  req_attach.entry.addr = (uint32_t)framebuffer;
  req_attach.entry.length = screen_w * screen_h * 4;
  virtio_gpu_send_req(&req_attach, sizeof(req_attach));

  // 3. スキャンアウト設定
  struct virtio_gpu_set_scanout set_scanout = {0};
  set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  set_scanout.r.x = 0;
  set_scanout.r.y = 0;
  set_scanout.r.width = screen_w;
  set_scanout.r.height = screen_h;
  set_scanout.scanout_id = 0;
  set_scanout.resource_id = 1;
  virtio_gpu_send_req(&set_scanout, sizeof(set_scanout));

  // 画面を青で塗りつぶすテスト
  for (uint32_t y = 0; y < screen_h; y++) {
    for (uint32_t x = 0; x < screen_w; x++) {
      framebuffer[y * screen_w + x] =
          0xFF0000FF; // ABGR: Red=00, Green=00, Blue=FF
    }
  }

  // 文字描画テスト
  draw_string("Hello Virtio-GPU World!", 50, 50, 0xFFFFFFFF); // White text
  draw_string("minOS is running...", 50, 70, 0xFF00FF00);     // Green text

  // 4. 転送とフラッシュ
  struct virtio_gpu_transfer_to_host_2d transfer = {0};
  transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  transfer.r.x = 0;
  transfer.r.y = 0;
  transfer.r.width = screen_w;
  transfer.r.height = screen_h;
  transfer.offset = 0;
  transfer.resource_id = 1;
  virtio_gpu_send_req(&transfer, sizeof(transfer));

  struct virtio_gpu_resource_flush flush = {0};
  flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  flush.r.x = 0;
  flush.r.y = 0;
  flush.r.width = screen_w;
  flush.r.height = screen_h;
  flush.resource_id = 1;
  virtio_gpu_send_req(&flush, sizeof(flush));

  printf("GPU Initialized. Screen cleared to Blue.\n");
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      if (x + dx < (int)screen_w && y + dy < (int)screen_h) {
        framebuffer[(y + dy) * screen_w + (x + dx)] = color;
      }
    }
  }
}

void virtio_gpu_flush(void) {
  struct virtio_gpu_transfer_to_host_2d transfer = {0};
  transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  transfer.r.x = 0;
  transfer.r.y = 0;
  transfer.r.width = screen_w;
  transfer.r.height = screen_h;
  transfer.offset = 0;
  transfer.resource_id = 1;
  virtio_gpu_send_req(&transfer, sizeof(transfer));

  struct virtio_gpu_resource_flush flush = {0};
  flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  flush.r.x = 0;
  flush.r.y = 0;
  flush.r.width = screen_w;
  flush.r.height = screen_h;
  flush.resource_id = 1;
  virtio_gpu_send_req(&flush, sizeof(flush));
}

void draw_cursor(int prev_x, int prev_y, int new_x, int new_y) {
  // 古いカーソルを消す（青で塗りつぶす）
  draw_rect(prev_x, prev_y, 10, 10, 0xFF0000FF);

  // 新しいカーソルを描く（白）
  draw_rect(new_x, new_y, 10, 10, 0xFFFFFFFF);

  // 画面更新
  virtio_gpu_flush();
}

// 8進数文字列を整数に変換
int oct2int(char *oct, int len) {
  int dec = 0;
  for (int i = 0; i < len; i++) {
    if (oct[i] < '0' || oct[i] > '7')
      break;

    dec = dec * 8 + (oct[i] - '0');
  }
  return dec;
}

// ファイルシステムの初期化
void fs_init(void) {
  for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
    read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);

  unsigned off = 0;
  for (int i = 0; i < FILES_MAX; i++) {
    struct tar_header *header = (struct tar_header *)&disk[off];
    if (header->name[0] == '\0')
      break;

    if (strcmp(header->magic, "ustar") != 0)
      PANIC("invalid tar header: magic=\"%s\"", header->magic);

    int filesz = oct2int(header->size, sizeof(header->size));
    struct file *file = &files[i];
    file->in_use = true;
    strcpy(file->name, header->name);
    memcpy(file->data, header->data, filesz);
    file->size = filesz;
    printf("file: %s, size=%d\n", file->name, file->size);

    off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
  }
}

// ファイルへの書き込み
void fs_flush(void) {
  // files変数の各ファイルの内容をdisk変数に書き込む
  memset(disk, 0, sizeof(disk));
  unsigned off = 0;
  for (int file_i = 0; file_i < FILES_MAX; file_i++) {
    struct file *file = &files[file_i];
    if (!file->in_use)
      continue;

    struct tar_header *header = (struct tar_header *)&disk[off];
    memset(header, 0, sizeof(*header));
    strcpy(header->name, file->name);
    strcpy(header->mode, "000644");
    strcpy(header->magic, "ustar");
    strcpy(header->version, "00");
    header->type = '0';

    // ファイルサイズを8進数文字列に変換
    int filesz = file->size;
    for (int i = sizeof(header->size); i > 0; i--) {
      header->size[i - 1] = (filesz % 8) + '0';
      filesz /= 8;
    }

    // チェックサムを計算
    int checksum = ' ' * sizeof(header->checksum);
    for (unsigned i = 0; i < sizeof(struct tar_header); i++)
      checksum += (unsigned char)disk[off + i];

    for (int i = 5; i >= 0; i--) {
      header->checksum[i] = (checksum % 8) + '0';
      checksum /= 8;
    }

    // ファイルデータをコピー
    memcpy(header->data, file->data, file->size);
    off += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
  }

  // disk変数の内容をディスクに書き込む
  for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
    read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);

  printf("wrote %d bytes to disk\n", sizeof(disk));
}

void virtio_input_init(void) {
  uint32_t *paddr = (uint32_t *)VIRTIO_BLK_PADDR;
  for (int i = 0; i < 8; i++) {
    uint32_t magic = paddr[0];
    uint32_t device_id = paddr[2];

    if (magic == 0x74726976 && device_id == VIRTIO_DEVICE_INPUT) {
      // Inputデバイス発見。サブタイプを確認
      // Virtio-InputのConfig領域([0x100...])を読み取る
      // 簡易的にIRQ番号(i+1)でKeyboardかMouseか判断(QEMUの引数順に依存)
      uint32_t irq = i + 1;
      uint32_t base = (uint32_t)paddr;

      virtio_reg_write32(base, VIRTIO_REG_DEVICE_STATUS, 0);
      virtio_reg_fetch_and_or32(base, VIRTIO_REG_DEVICE_STATUS,
                                VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
      virtio_reg_write32(base, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

      struct virtio_virtq *vq = virtq_init(base, 0); // Event Queue (index 0)

      if (irq == VIRTIO_KEYBOARD_IRQ) {
        printf("found keyboard at %x\n", base);
        keyboard_paddr = base;
        keyboard_vq = vq;
        // イベントバッファをセット
        for (int j = 0; j < VIRTQ_ENTRY_NUM; j++) {
          vq->descs[j].addr = (uint32_t)&keyboard_event_bufs[j];
          vq->descs[j].len = sizeof(struct virtio_input_event);
          vq->descs[j].flags = VIRTQ_DESC_F_WRITE;
          vq->avail.ring[j] = j;
        }
        vq->avail.index = VIRTQ_ENTRY_NUM;
      } else if (irq == VIRTIO_MOUSE_IRQ) {
        printf("found mouse at %x\n", base);
        mouse_paddr = base;
        mouse_vq = vq;
        // イベントバッファをセット
        for (int j = 0; j < VIRTQ_ENTRY_NUM; j++) {
          vq->descs[j].addr = (uint32_t)&mouse_event_bufs[j];
          vq->descs[j].len = sizeof(struct virtio_input_event);
          vq->descs[j].flags = VIRTQ_DESC_F_WRITE;
          vq->avail.ring[j] = j;
        }
        vq->avail.index = VIRTQ_ENTRY_NUM;
      }

      __sync_synchronize();
      virtio_reg_write32(base, VIRTIO_REG_QUEUE_NOTIFY, 0);
      virtio_reg_write32(base, VIRTIO_REG_DEVICE_STATUS,
                         VIRTIO_STATUS_DRIVER_OK);
    }
    paddr = (uint32_t *)((uint32_t)paddr + 0x1000);
  }
}

// カーネルメイン
void kernel_main(void) {
  // BSS領域をゼロ初期化
  memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

  // Hello World!テスト
  const char *s = "\n\nHello World!\n";
  for (int i = 0; s[i] != '\0'; i++) {
    putchar(s[i]);
  }

  // printfテスト
  printf("\n\nHello %s (printf)\n", "World!");
  printf("1 + 2 = %d, %x\n", 1 + 2, 0x1234abcd);

  // メモリ割当テスト
  paddr_t paddr0 = alloc_pages(2);
  paddr_t paddr1 = alloc_pages(1);
  printf("alloc_pages test: paddr0=%x\n", paddr0);
  printf("alloc_pages test: paddr1=%x\n", paddr1);

  // 例外処理テスト
  WRITE_CSR(stvec, (uint32_t)kernel_entry);

  // PLIC初期化
  plic_init();

  // VIRTIO初期化
  virtio_blk_init();

  // GPU初期化 (プロービングのみ)
  virtio_gpu_init();

  // Input初期化
  virtio_input_init();

  // ファイルシステム初期化
  fs_init();

  // ディスクの内容を確認
  char buf[SECTOR_SIZE];
  read_write_disk(buf, 0, false);
  printf("first sector: %s\n", buf);

  strcpy(buf, "hello from kernel!!!\n");
  read_write_disk(buf, 0, true);

  // IDLEプロセス生成
  idle_proc = create_process(NULL, 0);
  idle_proc->pid = 0;
  current_proc = idle_proc;

  // プロセス生成と起動
  create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_size);
  // proc_a = create_process((uint32_t)proc_a_entry);
  // proc_b = create_process((uint32_t)proc_b_entry);

  // スケジューラを呼び出し
  yield();
  PANIC("switched to idle process");

  // 未定義例外を発生させる(到達しない)
  __asm__ __volatile__("unimp");

  // ここにも到達しない
  PANIC("booted!");
  printf("unreachable here!\n");

  // ここにも到達しない
  while (1) {
    __asm__ __volatile__("wfi");
  }
}

// ブートコード
__attribute__((section(".text.boot"))) __attribute__((naked)) void boot(void) {
  __asm__ __volatile__("mv sp, %[stack_top]\n"
                       "j kernel_main\n"
                       :
                       : [stack_top] "r"(__stack_top));
}
