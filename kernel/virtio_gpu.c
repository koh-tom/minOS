#include "common.h"
#include "kernel.h"

uint32_t virtio_gpu_paddr = 0;
struct virtio_virtq *gpu_control_vq;
struct virtio_gpu_config *gpu_config;
uint32_t screen_w = 640;
uint32_t screen_h = 480;
uint32_t *framebuffer;

extern uint8_t font_bitmap[256][16];

void virtio_gpu_send_req(void *req, int len) {
  struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)req;
  hdr->flags = VIRTIO_GPU_FLAG_FENCE;
  hdr->fence_id = 0;
  hdr->ctx_id = 0;

  struct virtio_virtq *virtq = gpu_control_vq;
  virtq->descs[0].addr = (uint32_t)req;
  virtq->descs[0].len = len;
  virtq->descs[0].flags = VIRTQ_DESC_F_NEXT;
  virtq->descs[0].next = 1;

  virtq->descs[1].addr = (uint32_t)req;
  virtq->descs[1].len = sizeof(struct virtio_gpu_ctrl_hdr);
  virtq->descs[1].flags = VIRTQ_DESC_F_WRITE;
  virtq->descs[1].next = 0;

  virtq->avail.ring[virtq->avail.index % VIRTQ_ENTRY_NUM] = 0;
  virtq->avail.index++;
  __sync_synchronize();
  virtio_reg_write32(virtq->reg_base, VIRTIO_REG_QUEUE_NOTIFY,
                     virtq->queue_index);
  virtq->last_used_index++;

  while (virtq->last_used_index != *virtq->used_index) {
  }
}

void virtio_gpu_init(void) {
  printf("Probing for Virtio-GPU...\n");
  uint32_t *paddr = (uint32_t *)VIRTIO_BLK_PADDR;
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

  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS, 0);
  virtio_reg_fetch_and_or32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_ACK);
  virtio_reg_fetch_and_or32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_DRIVER);

  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

  gpu_control_vq = virtq_init(virtio_gpu_paddr, 0);

  virtio_reg_write32(virtio_gpu_paddr, VIRTIO_REG_DEVICE_STATUS,
                     VIRTIO_STATUS_DRIVER_OK);

  framebuffer = (uint32_t *)alloc_pages(
      align_up(screen_w * screen_h * 4, PAGE_SIZE) / PAGE_SIZE);
  memset(framebuffer, 0, screen_w * screen_h * 4);

  struct virtio_gpu_resource_create_2d res_create = {0};
  res_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  res_create.resource_id = 1;
  res_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  res_create.width = screen_w;
  res_create.height = screen_h;
  virtio_gpu_send_req(&res_create, sizeof(res_create));

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

  struct virtio_gpu_set_scanout set_scanout = {0};
  set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  set_scanout.r.x = 0;
  set_scanout.r.y = 0;
  set_scanout.r.width = screen_w;
  set_scanout.r.height = screen_h;
  set_scanout.scanout_id = 0;
  set_scanout.resource_id = 1;
  virtio_gpu_send_req(&set_scanout, sizeof(set_scanout));

  for (uint32_t y = 0; y < screen_h; y++) {
    for (uint32_t x = 0; x < screen_w; x++) {
      framebuffer[y * screen_w + x] = 0xFF0000FF;
    }
  }

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

void virtio_gpu_flush_smart(int x, int y, int w, int h) {
  struct virtio_gpu_transfer_to_host_2d transfer = {0};
  transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  transfer.r.x = x;
  transfer.r.y = y;
  transfer.r.width = w;
  transfer.r.height = h;
  transfer.offset = (y * screen_w + x) * 4;
  transfer.resource_id = 1;
  virtio_gpu_send_req(&transfer, sizeof(transfer));

  struct virtio_gpu_resource_flush flush = {0};
  flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  flush.r.x = x;
  flush.r.y = y;
  flush.r.width = w;
  flush.r.height = h;
  flush.resource_id = 1;
  virtio_gpu_send_req(&flush, sizeof(flush));
}

void virtio_gpu_flush(void) {
  virtio_gpu_flush_smart(0, 0, screen_w, screen_h);
}

void draw_char(char c, int x, int y, uint32_t color) {
  for (int dy = 0; dy < 16; dy++) {
    uint8_t row = font_bitmap[(uint8_t)c][dy];
    for (int dx = 0; dx < 8; dx++) {
      if ((row >> (7 - dx)) & 1) {
        if (x + dx < (int)screen_w && y + dy < (int)screen_h) {
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
  virtio_gpu_flush();
}

void draw_cursor(int prev_x, int prev_y, int new_x, int new_y) {
  draw_rect(prev_x, prev_y, 10, 10, 0xFF0000FF);
  virtio_gpu_flush_smart(prev_x, prev_y, 10, 10);

  draw_rect(new_x, new_y, 10, 10, 0xFFFFFFFF);
  virtio_gpu_flush_smart(new_x, new_y, 10, 10);
}
