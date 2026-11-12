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
/* 1 if rc proves the request had NO effect on the device (rejected before submission); 0 if the outcome is unknown
 * (I/O error, timeout, protocol error: the write may or may not have reached stable storage). BLK_OK is not an error. */
int blk_error_definite(int rc);

/* Diagnostics for tests/logs. */
struct blk_info { uint64_t base; uint32_t version, intid, features_hi, features_lo; int nodes_seen, nodes_blk; };
const struct blk_info *blk_get_info(void);
uint64_t blk_requests(void);
uint64_t blk_timeouts(void);
void     blk_test_stall_next(int on);                          /* tests only: skip the doorbell so the next request must time out */
void     blk_test_set_timeout_ms(uint32_t ms);
void     blk_test_drop_writes_after(int n);                    /* tests only: after n more writes, writes/flushes silently do nothing (a power cut); n < 0 = off */
uint64_t blk_writes_done(void);
/* Tests only: fail the nth upcoming write request (n = 0 is the next one) / flush request, exactly once.
 *   REJECT       definite failure: the device never sees it (returns BLK_EARG)
 *   FAIL_UNSENT  the request is NOT sent to the device but BLK_EIO is returned (an error whose outcome the caller cannot know)
 *   FAIL_SENT    the request IS performed by the device, then BLK_EIO is returned (completion reported as an error after the data landed) */
enum { BLK_INJ_REJECT = 1, BLK_INJ_FAIL_UNSENT = 2, BLK_INJ_FAIL_SENT = 3 };
void blk_test_inject_write(int kind, int nth);
void blk_test_inject_flush(int kind, int nth);
void blk_test_inject_clear(void);                                /* write requests actually sent to the device */

void blktest_run(void);        /* in-kernel tests; a note only when no disk is attached */
int  blktest_summary(void);
#endif
