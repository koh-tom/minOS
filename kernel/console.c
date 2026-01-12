#include "common.h"
#include "kernel.h"

int cursor_x = 0;
int cursor_y = 0;

void console_putchar(char c) {
  if (!virtio_gpu_paddr)
    return;

  if (c == '\n') {
    cursor_x = 0;
    cursor_y += 16;
  } else if (c == '\b') {
    if (cursor_x > 0) {
      cursor_x--;
      draw_rect(cursor_x * 8, cursor_y, 8, 16, 0xFF0000FF);
      virtio_gpu_flush_smart(cursor_x * 8, cursor_y, 8, 16);
    }
  } else {
    draw_char(c, cursor_x * 8, cursor_y, 0xFFFFFFFF);
    cursor_x++;
    virtio_gpu_flush_smart((cursor_x - 1) * 8, cursor_y, 8, 16);
  }

  if (cursor_x * 8 >= (int)screen_w) {
    cursor_x = 0;
    cursor_y += 16;
  }

  if (cursor_y + 16 >= (int)screen_h) {
    cursor_x = 0;
    cursor_y = 0;
    draw_rect(0, 0, screen_w, screen_h, 0xFF0000FF);
    virtio_gpu_flush();
  }
}

void putchar(char ch) {
  sbi_call(ch, 0, 0, 0, 0, 0, 0, 1);
  console_putchar(ch);
}
