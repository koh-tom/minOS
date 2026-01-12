#include "kernel.h"

void plic_init(void) {
  for (int i = 1; i <= 32; i++) {
    virtio_reg_write32(PLIC_PRIORITY(i), 0, 1);
  }

  virtio_reg_write32(PLIC_SENABLE(0, 0), 0,
                     (1 << VIRTIO_KEYBOARD_IRQ) | (1 << VIRTIO_MOUSE_IRQ));

  virtio_reg_write32(PLIC_SPRIORITY(0), 0, 0);

  WRITE_CSR(sie, READ_CSR(sie) | SIE_SEIE);
  WRITE_CSR(sstatus, READ_CSR(sstatus) | SSTATUS_SIE);
}
