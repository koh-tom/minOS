#include "common.h"
#include "kernel.h"

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

struct virtio_virtq *virtq_init(uint32_t base, unsigned index) {
  paddr_t virtq_paddr =
      alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
  struct virtio_virtq *virtq = (struct virtio_virtq *)virtq_paddr;
  virtq->queue_index = index;
  virtq->used_index = (volatile uint16_t *)&virtq->used.index;
  virtq->reg_base = base;

  virtio_reg_write32(base, VIRTIO_REG_QUEUE_SEL, index);
  virtio_reg_write32(base, VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
  virtio_reg_write32(base, VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
  return virtq;
}

void virtq_kick(struct virtio_virtq *virtq, int desc_index) {
  virtq->avail.ring[virtq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
  virtq->avail.index++;
  __sync_synchronize();
  virtio_reg_write32(virtq->reg_base, VIRTIO_REG_QUEUE_NOTIFY,
                     virtq->queue_index);
  virtq->last_used_index++;
}

bool virtq_is_busy(struct virtio_virtq *virtq) {
  return virtq->last_used_index != *virtq->used_index;
}
