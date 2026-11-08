#ifndef AURORA_BLK_H
#define AURORA_BLK_H
#include <stdint.h>
#include <mm.h>

/* Kernel block-device interface (512-byte sectors). Backed by a virtio-blk device found through the device tree. */
#define BLK_SECTOR 512

enum { BLK_OK = 0, BLK_EARG = -1, BLK_ERANGE = -2, BLK_ENODEV = -3, BLK_ERO = -4, BLK_EIO = -5, BLK_ETIMEOUT = -6, BLK_EPROTO = -7 };

/* Scan the device tree for virtio-mmio transports and bring up the first block device.
 * Returns BLK_OK, BLK_ENODEV (no block device present: a normal, supported situation) or BLK_EPROTO (device present but unusable,
 * e.g. legacy transport, no VERSION_1, feature negotiation refused). */
int      blk_init(const void *fdt);
int      blk_present(void);
uint64_t blk_sectors(void);                                   /* capacity in 512-byte sectors */
int      blk_readonly(void);
int      blk_read(uint64_t sector, void *buf, uint32_t nsect);
int      blk_write(uint64_t sector, const void *buf, uint32_t nsect);
int      blk_flush(void);                                     /* BLK_OK also when the device has no volatile cache to flush */
const char *blk_strerror(int rc);

/* Diagnostics for tests/logs. */
struct blk_info { uint64_t base; uint32_t version, intid, features_hi, features_lo; int nodes_seen, nodes_blk; };
const struct blk_info *blk_get_info(void);
uint64_t blk_requests(void);
uint64_t blk_timeouts(void);
void     blk_test_stall_next(int on);                          /* tests only: skip the doorbell so the next request must time out */
void     blk_test_set_timeout_ms(uint32_t ms);
void     blk_test_drop_writes_after(int n);                    /* tests only: after n more writes, writes/flushes silently do nothing (a power cut); n < 0 = off */
uint64_t blk_writes_done(void);                                /* write requests actually sent to the device */

void blktest_run(void);        /* in-kernel tests; a note only when no disk is attached */
int  blktest_summary(void);
#endif
