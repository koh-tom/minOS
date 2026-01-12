#include "common.h"
#include "kernel.h"

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
        const char *name = files[i].name;
        if (strcmp(name, ".") == 0)
          continue;
        if (name[0] == '.' && name[1] == '/')
          name += 2;
        printf("%s  ", name);
      }
    }
    printf("\n");
    break;
  }
  case SYS_PS: {
    printf("PID  STATE\n");
    for (int i = 0; i < PROCS_MAX; i++) {
      struct process *proc = &procs[i];
      if (proc->state == PROCS_UNUSED)
        continue;

      const char *state = "UNKNOWN";
      if (proc->state == PROCS_RUNNABLE)
        state = "RUNNABLE";
      else if (proc->state == PROC_EXITED)
        state = "EXITED";

      printf("%d    %s\n", proc->pid, state);
    }
    break;
  }
  case SYS_SBRK: {
    int increment = f->a0;
    uintptr_t old_brk = current_proc->brk;
    uintptr_t new_brk = old_brk + increment;

    if (increment == 0) {
      f->a0 = old_brk;
      break;
    }

    // 新しいページを割り当ててマッピング
    uintptr_t start_page = align_up(old_brk, PAGE_SIZE);
    uintptr_t end_page = align_up(new_brk, PAGE_SIZE);

    for (uintptr_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
      paddr_t page = alloc_pages(1);
      map_page(current_proc->page_table, addr, page, PAGE_U | PAGE_R | PAGE_W);
    }

    current_proc->brk = new_brk;
    f->a0 = old_brk;
    break;
  }
  default:
    PANIC("unexpected syscall a3=%x\n", f->a3);
  }
}

void handle_trap(struct trap_frame *f) {
  uint32_t scause = READ_CSR(scause);
  uint32_t stval = READ_CSR(stval);
  uint32_t user_pc = READ_CSR(sepc);

  if (scause == SCAUSE_ECALL) {
    handle_syscall(f);
    user_pc += 4;
  } else if (scause == SCAUSE_EXTERNAL_INTERRUPT) {
    // S-mode External Interrupt
    uint32_t irq = virtio_reg_read32(PLIC_SCLAIM(0), 0);

    if (irq == VIRTIO_KEYBOARD_IRQ) {
      handle_keyboard_interrupt();
    } else if (irq == VIRTIO_MOUSE_IRQ) {
      handle_mouse_interrupt();
    }

    if (irq) {
      virtio_reg_write32(PLIC_SCLAIM(0), 0, irq);
    }
  } else if (scause == SCAUSE_TIMER_INTERRUPT) {
    // タイマー割り込み: 次の割り込みをセットしてコンテキストスイッチ
    sbi_call(0, 0, 0, 0, 0, 0, 0, 0); // タイマーリセット (OpenSBI依存)

    // 次のタイマー設定 (10ms後)
    uint64_t current_time;
    __asm__ volatile("rdtime %0" : "=r"(current_time));
    sbi_call(current_time + 100000, 0, 0, 0, 0, 0, 0, 0); // 100000tick後

    yield();
  } else {
    PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval,
          user_pc);
  }
  WRITE_CSR(sepc, user_pc);
}

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
