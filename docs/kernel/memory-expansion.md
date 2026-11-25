# Memory Expansion (MEM-01/MEM-02)

## Plan Outline

### MEM-01: Configurable QEMU Memory Values
- Memory size passed via QEMU `-m` flag during boot
- Default: 256M, scalable up to 1G+ depending on host resources
- Memory reservation table in device tree for consistent guest-visible layout
- Guard pages and margin allocation for kernel reserve
- Hot-unplug support via virtio memory balloon (future)

### MEM-02: Scalable Ownership Design
- Per-process address space isolation with kernel-managed page tables
- Principal-based memory quotas enforceable through syscall checks
- Kernel reserve pool for exceptions, IRQ stacks, and framebuffer
- Transparent swapping to backing store when physical pressure exceeds threshold
- Auditable memory allocation events logged to the audit ledger (Trail)

### Ownership Notes
- Memory ownership follows the same principal model as AuroraFS: each mapped page is tagged with owning process/principal
- Reference counting via kernel-owned structures; free when refcount drops to zero
- No cross-principal aliasing: MMU policy refuses unsafe aliases (writable+executable, device-mapped without no-map flag)
- Memory expansion is transparent to EL0 apps; syscall interface unchanged
- QEMU can override via firmware cfg, but guest kernel makes final decision

### Boot-time Flow
1. QEMU passes `-m SIZE` to kernel via device tree `/memory-region`
2. Kernel bitmap allocator initializes within reserved range
3. Stage-1 MMU setup respects W^X and guard-page policy
4. Process 0 (init) gets default quota; other processes request via `SYS_MEM`