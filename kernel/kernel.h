#pragma once
#include "common.h"

#define PROCS_MAX 8
#define PROCS_UNUSED 0
#define PROCS_RUNNABLE 1
#define PROC_EXITED 2

#define SATP_SV32 (1u << 31)
#define PAGE_V (1 << 0)
#define PAGE_R (1 << 1)
#define PAGE_W (1 << 2)
#define PAGE_X (1 << 3)
#define PAGE_U (1 << 4)
#define SSTATUS_SPIE (1 << 5)
#define SSTATUS_SUM (1 << 18)

#define USER_BASE 0x1000000
#define SCAUSE_ECALL 8
#define FILES_MAX 10
#define DISK_MAX_SIZE align_up(sizeof(struct file) * FILES_MAX, PAGE_SIZE)

// VIRTIO
#define SECTOR_SIZE 512
#define VIRTQ_ENTRY_NUM 16
#define VIRTIO_DEVICE_BLK 2
#define VIRTIO_DEVICE_GPU 16
#define VIRTIO_DEVICE_INPUT 18
#define VIRTIO_BLK_PADDR 0x10001000
#define VIRTIO_REG_MAGIC 0x00
#define VIRTIO_REG_VERSION 0x04
#define VIRTIO_REG_DEVICE_ID 0x08
#define VIRTIO_REG_PAGE_SIZE 0x28
#define VIRTIO_REG_QUEUE_SEL 0x30
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34
#define VIRTIO_REG_QUEUE_NUM 0x38
#define VIRTIO_REG_QUEUE_PFN 0x40
#define VIRTIO_REG_QUEUE_READY 0x44
#define VIRTIO_REG_QUEUE_NOTIFY 0x50
#define VIRTIO_REG_DEVICE_STATUS 0x70
#define VIRTIO_REG_DEVICE_CONFIG 0x100

// PLIC
#define PLIC_BASE 0x0c000000
#define PLIC_PRIORITY(irq) (PLIC_BASE + (irq) * 4)
#define PLIC_PENDING(irq) (PLIC_BASE + 0x1000 + ((irq) / 32) * 4)
#define PLIC_MENABLE(hart, irq)                                                \
  (PLIC_BASE + 0x2000 + (hart) * 0x80 + ((irq) / 32) * 4)
#define PLIC_SENABLE(hart, irq)                                                \
  (PLIC_BASE + 0x2080 + (hart) * 0x100 + ((irq) / 32) * 4)
#define PLIC_MPRIORITY(hart) (PLIC_BASE + 0x200000 + (hart) * 0x2000)
#define PLIC_SPRIORITY(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)
#define PLIC_MCLAIM(hart) (PLIC_BASE + 0x200004 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart) (PLIC_BASE + 0x201004 + (hart) * 0x2000)

// 割り込み
#define VIRTIO_BLK_IRQ 1
#define VIRTIO_GPU_IRQ 2
#define VIRTIO_KEYBOARD_IRQ 3
#define VIRTIO_MOUSE_IRQ 4

#define SCAUSE_INTERRUPT 0x80000000

#define SSTATUS_SIE (1 << 1)
#define SIE_SEIE (1 << 9)
#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

// マクロ
#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

#define READ_CSR(reg)                                                          \
  ({                                                                           \
    unsigned long __tmp;                                                       \
    __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));                      \
    __tmp;                                                                     \
  })

#define WRITE_CSR(reg, value)                                                  \
  do {                                                                         \
    uint32_t __tmp = (value);                                                  \
    __asm__ __volatile__("csrw " #reg ", %0" ::"r"(__tmp));                    \
  } while (0)

struct sbiret {
  long error;
  long value;
};

struct trap_frame {
  uint32_t ra, gp, tp, t0, t1, t2, t3, t4, t5, t6;
  uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
  uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
  uint32_t sp;
} __attribute__((packed));

struct process {
  int pid;
  int state;
  vaddr_t sp;
  uint32_t *page_table;
  uint8_t stack[8192];
};

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct virtq_avail {
  uint16_t flags;
  uint16_t index;
  uint16_t ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));

struct virtq_used {
  uint16_t flags;
  uint16_t index;
  struct virtq_used_elem ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtio_virtq {
  struct virtq_desc descs[VIRTQ_ENTRY_NUM];
  struct virtq_avail avail;
  struct virtq_used used __attribute__((aligned(PAGE_SIZE)));
  int queue_index;
  volatile uint16_t *used_index;
  uint16_t last_used_index;
  uint32_t reg_base;
} __attribute__((packed));

struct virtio_blk_req {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
  uint8_t data[512];
  uint8_t status;
} __attribute__((packed));

// VIRTIO-GPU
#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)

struct virtio_gpu_config {
  uint32_t events_read;
  uint32_t events_clear;
  uint32_t num_scanouts;
  uint32_t num_capsets;
} __attribute__((packed));

enum virtio_gpu_ctrl_type {
  VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
  VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
  VIRTIO_GPU_CMD_RESOURCE_UNREF,
  VIRTIO_GPU_CMD_SET_SCANOUT,
  VIRTIO_GPU_CMD_RESOURCE_FLUSH,
  VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
  VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
  VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
  VIRTIO_GPU_CMD_GET_CAPSET_INFO,
  VIRTIO_GPU_CMD_GET_CAPSET,
  VIRTIO_GPU_CMD_GET_EDID,
  VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
};

#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

struct virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
  uint32_t x, y, width, height;
} __attribute__((packed));

enum virtio_gpu_formats {
  VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
};

struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
  uint64_t addr;
  uint32_t length;
  uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t scanout_id;
  uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

struct virtio_input_event {
  uint16_t type;
  uint16_t code;
  uint32_t value;
} __attribute__((packed));

struct tar_header {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char type;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char padding[12];
  char data[];
} __attribute__((packed));

struct file {
  bool in_use;
  char name[100];
  char data[1024];
  size_t size;
};

extern struct process procs[PROCS_MAX];
extern struct process *current_proc;
extern struct process *idle_proc;
extern char __bss[], __bss_end[], __stack_top[];
extern char __free_ram[], __free_ram_end[];
extern char __kernel_base[];
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];
extern struct file files[FILES_MAX];
extern uint8_t disk[DISK_MAX_SIZE];
extern uint32_t screen_w;
extern uint32_t screen_h;
extern uint32_t *framebuffer;
extern int cursor_x;
extern int cursor_y;
extern int mouse_x;
extern int mouse_y;

// alloc.c
paddr_t alloc_pages(uint32_t n);
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags);
void *kmalloc(size_t size);
void kfree(void *ptr);

// proc.c
struct process *create_process(const void *image, size_t image_size);
void yield(void);
void switch_context(uint32_t *prev_sp, uint32_t *next_sp);
void user_entry(void);

// trap.c
void handle_trap(struct trap_frame *f);
void kernel_entry(void);
struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid);

// plic.c
void plic_init(void);

// virtio.c
struct virtio_virtq *virtq_init(uint32_t base, unsigned index);
uint32_t virtio_reg_read32(uint32_t base, unsigned offset);
uint64_t virtio_reg_read64(uint32_t base, unsigned offset);
void virtio_reg_write32(uint32_t base, unsigned offset, uint32_t value);
void virtio_reg_fetch_and_or32(uint32_t base, unsigned offset, uint32_t value);
void virtq_kick(struct virtio_virtq *virtq, int desc_index);
bool virtq_is_busy(struct virtio_virtq *virtq);

// virtio_blk.c
void virtio_blk_init(void);
void read_write_disk(void *buf, unsigned sector, int is_write);
extern uint64_t blk_capacity;

// virtio_gpu.c
void virtio_gpu_init(void);
void virtio_gpu_send_req(void *req, int len);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_char(char c, int x, int y, uint32_t color);
void draw_string(const char *s, int x, int y, uint32_t color);
void virtio_gpu_flush(void);
void virtio_gpu_flush_smart(int x, int y, int w, int h);
void draw_cursor(int prev_x, int prev_y, int new_x, int new_y);
extern uint32_t virtio_gpu_paddr;

// virtio_input.c
void virtio_input_init(void);
void handle_keyboard_interrupt(void);
void handle_mouse_interrupt(void);
long getchar(void);
extern struct virtio_virtq *keyboard_vq;
extern struct virtio_virtq *mouse_vq;

// fs.c
void fs_init(void);
void fs_flush(void);
struct file *fs_lookup(const char *filename);

// console.c
void console_putchar(char c);
void putchar(char ch);