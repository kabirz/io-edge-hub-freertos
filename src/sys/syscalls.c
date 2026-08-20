/* 最小 newlib syscalls 桩 (STM32::NoSys)。
 * _write (printf -> USART1 阻塞发送) 已移交 src/sys/log.c (与 log_write
 * 同一 UART 出口、同一把日志锁); 此处保留其余桩。 */
#include <sys/stat.h>
#include <errno.h>

int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
caddr_t _sbrk(int incr) { (void)incr; errno = ENOMEM; return (caddr_t)-1; } /* 全静态分配，无堆 */
