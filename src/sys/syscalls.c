#include <sys/stat.h>
#include <errno.h>
#include "main.h"   /* extern UART 句柄，见 Step 9 main.c 定义 */

/* 最小 syscalls：printf -> USART1 阻塞发送 */
int _write(int fd, const char *buf, int len);
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
caddr_t _sbrk(int incr) { (void)incr; errno = ENOMEM; return (caddr_t)-1; } /* 全静态分配，无堆 */

extern UART_HandleTypeDef huart1;
int _write(int fd, const char *buf, int len)
{
    (void)fd;
    HAL_UART_Transmit(&huart1, (const uint8_t *)buf, (uint16_t)len, 100);
    return len;
}
