#include "user.h"

void main(void) {
  while (1) {
  prompt:
    printf("minOS>");
    char cmdline[128];
    int i = 0;

    for (;;) {
      char ch = getchar();

      // Enterキー
      if (ch == '\n' || ch == '\r') {
        putchar('\n');
        cmdline[i] = '\0';
        break;
      }

      // Backspaceキー
      if (ch == '\b' || ch == 0x7f) {
        if (i > 0) {
          i--;
          // 画面上の1文字削除
          putchar('\b');
          putchar(' ');
          putchar('\b');
        }
        continue;
      }

      // バッファオーバーフロー防止
      if (i >= sizeof(cmdline) - 1) {
        printf("\ncommand line too long\n");
        goto prompt;
      }

      // 通常文字
      cmdline[i++] = ch;
      putchar(ch);
    }

    if (strcmp(cmdline, "hello") == 0) {
      printf("Hello World from shell!\n");
    } else if (strcmp(cmdline, "exit") == 0) {
      exit();
    } else if (strcmp(cmdline, "readfile") == 0) {
      char buf[128];
      int len = readfile("hello.txt", buf, sizeof(buf) - 1);
      buf[len] = '\0';
      printf("%s\n", buf);
    } else if (strcmp(cmdline, "writefile") == 0) {
      writefile("hello.txt", "Hello from shell!\n", 19);
    } else if (strcmp(cmdline, "ls") == 0) { // lsコマンド
      ls();
    } else if (cmdline[0] != '\0') {
      printf("unknown command: %s\n", cmdline);
    }
  }
}
