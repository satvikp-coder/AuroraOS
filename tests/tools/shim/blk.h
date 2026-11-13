/* Host shim for kernel/include/blk.h: just enough of the block interface for kernel/fs/afs.c to run over a plain file (tests/tools/mkafs.c). */
#ifndef AURORA_BLK_H
#define AURORA_BLK_H
#include <stdint.h>
#define BLK_SECTOR 512
enum { BLK_OK = 0, BLK_EARG = -1, BLK_ERANGE = -2, BLK_ENODEV = -3, BLK_ERO = -4, BLK_EIO = -5, BLK_ETIMEOUT = -6, BLK_EPROTO = -7 };
int      blk_present(void);
uint64_t blk_sectors(void);
int      blk_readonly(void);
int      blk_read(uint64_t sector, void *buf, uint32_t nsect);
int      blk_write(uint64_t sector, const void *buf, uint32_t nsect);
int      blk_flush(void);
int      blk_error_definite(int rc);
#endif
