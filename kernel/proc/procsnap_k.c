/* kernel/proc/procsnap_k.c - binds the pure snapshot (procsnap.c) to the kernel process table. Kept apart so host tests link procsnap.c alone. */
#include <procsnap.h>
uint32_t procsnap_take_kernel(struct procsnap *out) { return procsnap_take(out, (procsnap_get_fn)proc_get); }
