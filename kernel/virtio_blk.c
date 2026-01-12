#include "common.h"
#include "kernel.h"

struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

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

  virtq_kick(virtq, 0);

  while (virtq_is_busy(virtq)) {
  }

  if (blk_req->status != 0) {
    printf("virtio: warn: failed to read/write disk sector=%d status=%d\n",
           sector, blk_req->status);
    return;
  }

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

  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS, 0);
  virtio_reg_fetch_and_or32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_ACK);
  virtio_reg_fetch_and_or32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                            VIRTIO_STATUS_DRIVER);
  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

  blk_request_vq = virtq_init(VIRTIO_BLK_PADDR, 0);

  virtio_reg_write32(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_STATUS,
                     VIRTIO_STATUS_DRIVER_OK);

  blk_capacity =
      virtio_reg_read64(VIRTIO_BLK_PADDR, VIRTIO_REG_DEVICE_CONFIG + 0) *
      SECTOR_SIZE;
  printf("blk_capacity: %d\n", (int)blk_capacity);

  blk_req_paddr =
      alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
  blk_req = (struct virtio_blk_req *)blk_req_paddr;
}
