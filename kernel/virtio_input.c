#include "common.h"
#include "kernel.h"

struct virtio_virtq *keyboard_vq;
struct virtio_input_event keyboard_event_bufs[VIRTQ_ENTRY_NUM];
struct virtio_input_event keyboard_queue[64];
int keyboard_head = 0;
int keyboard_tail = 0;
uint32_t keyboard_paddr = 0;

struct virtio_virtq *mouse_vq;
struct virtio_input_event mouse_event_bufs[VIRTQ_ENTRY_NUM];
uint32_t mouse_paddr = 0;
int mouse_x = 0;
int mouse_y = 0;

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

long getchar(void) {
  struct virtio_input_event *event = keyboard_pop();
  if (event) {
    if ((event->value == 1 || event->value == 2) && event->type == 1) {
      int ch = key2char(event->code);
      if (ch)
        return ch;
    }
  }
  struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
  if (ret.error != -1) {
    return ret.error;
  }
  return -1;
}

void handle_keyboard_interrupt(void) {
  if (!keyboard_vq)
    return;
  while (keyboard_vq->last_used_index != *keyboard_vq->used_index) {
    struct virtq_used_elem *e =
        &keyboard_vq->used.ring[keyboard_vq->last_used_index % VIRTQ_ENTRY_NUM];
    uint32_t desc_idx = e->id;

    struct virtio_input_event *event =
        (struct virtio_input_event *)keyboard_vq->descs[desc_idx].addr;

    if (event->type == 1) {
      keyboard_push(*event);
    }

    keyboard_vq->avail.ring[keyboard_vq->avail.index % VIRTQ_ENTRY_NUM] =
        desc_idx;
    keyboard_vq->avail.index++;
    keyboard_vq->last_used_index++;
  }
  __sync_synchronize();
  virtio_reg_write32(keyboard_paddr, VIRTIO_REG_QUEUE_NOTIFY, 0);
}

void handle_mouse_interrupt(void) {
  if (!mouse_vq)
    return;
  static int prev_mouse_x = 0;
  static int prev_mouse_y = 0;
  bool updated = false;

  while (mouse_vq->last_used_index != *mouse_vq->used_index) {
    struct virtq_used_elem *e =
        &mouse_vq->used.ring[mouse_vq->last_used_index % VIRTQ_ENTRY_NUM];
    uint32_t desc_idx = e->id;

    struct virtio_input_event *event =
        (struct virtio_input_event *)mouse_vq->descs[desc_idx].addr;

    if (event->type == 3) {
      if (event->code == 0) { // ABS_X
        mouse_x = ((uint64_t)event->value * screen_w) / 32768;
        updated = true;
      } else if (event->code == 1) { // ABS_Y
        mouse_y = ((uint64_t)event->value * screen_h) / 32768;
        updated = true;
      }
    }

    if (mouse_x < 0)
      mouse_x = 0;
    if (mouse_x >= (int)screen_w)
      mouse_x = screen_w - 1;
    if (mouse_y < 0)
      mouse_y = 0;
    if (mouse_y >= (int)screen_h) // Fixed cast
      mouse_y = screen_h - 1;

    mouse_vq->avail.ring[mouse_vq->avail.index % VIRTQ_ENTRY_NUM] = desc_idx;
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

void virtio_input_init(void) {
  uint32_t *paddr = (uint32_t *)VIRTIO_BLK_PADDR;
  for (int i = 0; i < 8; i++) {
    uint32_t magic = paddr[0];
    uint32_t device_id = paddr[2];

    if (magic == 0x74726976 && device_id == VIRTIO_DEVICE_INPUT) {
      uint32_t irq = i + 1;
      uint32_t base = (uint32_t)paddr;

      virtio_reg_write32(base, VIRTIO_REG_DEVICE_STATUS, 0);
      virtio_reg_fetch_and_or32(base, VIRTIO_REG_DEVICE_STATUS,
                                VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
      virtio_reg_write32(base, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

      struct virtio_virtq *vq = virtq_init(base, 0);

      if (irq == VIRTIO_KEYBOARD_IRQ) {
        printf("found keyboard at %x\n", base);
        keyboard_paddr = base;
        keyboard_vq = vq;
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
