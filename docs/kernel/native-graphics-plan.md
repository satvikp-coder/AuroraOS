# AuroraOS native graphics: research and plan (issue #7, stage G1)

**Status: G1 research (sections 1 to 9) plus the G2 implementation record (section 10).** Sections 1 to 9 were written before any code and are kept as written; where section 10 verified a `[spec, recalled]` item, section 10 is authoritative. Everything targets QEMU `virt` only, run under `-cpu cortex-a53`; **nothing here was run on real hardware** and no claim is made about real hardware, GPUs or phones.

How each statement is known (marked in the text):

- **[verified]** run on this machine against the real QEMU, or read from the real device tree it produced (commands in the appendix).
- **[source]** read from this repository's kernel sources at commit `f4a70a4`.
- **[spec, recalled]** from the fw_cfg, VIRTIO or Arm specifications as I know them. **Not verified here. Re-verify every one in G2 against run-time behaviour before relying on it.**

## 1. Environment (checked before any work)

| Item | Value |
|---|---|
| Worktree | `C:\Users\satvi\Desktop\AuroraOS-Claude-kernel`, branch `research/native-graphics-qemu` created from `origin/main` at `f4a70a4`; clean tree before and after |
| Not touched | the sibling checkout `C:\Users\satvi\Desktop\AuroraOS`, any physical device |
| Toolchain (WSL `kali-linux`) | QEMU 11.1.0 (Debian 1:11.1.0+ds-2), aarch64 gcc 16.1.0, python3, nc, gcc/make; **no** `socat`, no image converters |
| Resources | WSL 3.9 GB RAM (about 2.8 GB available), 935 GB free disk. Windows had about 1.7 GB free, so heavy suites were not run concurrently |
| Boot flags used by every project test | `-machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -nographic -serial file:LOG -monitor none` (virtio devices add `-global virtio-mmio.force-legacy=false`) |

## 2. What the kernel provides today **[source]**

| Area | Today | Consequence for graphics |
|---|---|---|
| Output | PL011 UART at `0x09000000` only (`kernel/console.c`) | no display of any kind |
| MMU | TTBR0, 4 KiB granule, 39-bit VA, kernel **identity-mapped**; MAIR has only **Attr0 = Device-nGnRnE** and **Attr1 = Normal write-back**; `WXN` on (`docs/kernel/mmu-configuration.md`) | a framebuffer in RAM is already mapped Normal WB read/write, never executable; **there is no Normal non-cacheable attribute** |
| Mapping policy | `vmm_map` is checked by `mm_policy_check`; Device mappings only inside a window registered with `vmm_register_mmio()`; no bypass flag | any MMIO we use (fw_cfg, a virtio slot) must be registered first |
| Physical memory | `pmm_alloc_page()` hands out **single zeroed 4 KiB pages**; bitmap allocator capped at 1 GiB; `pmm_reserve_range()` can claim a range before it is allocated | **no contiguous allocation.** A 1024x768x4 framebuffer is 768 pages, so it must be carved out explicitly or the allocator extended |
| Device tree | `fdt.c` parses memory, reservations, GICv2, timer and the **virtio-mmio** nodes (`fdt_virtio_mmio`) | it does **not** parse `fw-cfg`; and the tree has **no framebuffer node at all** (section 3), so a display must be found by a device-specific driver |
| virtio | `kernel/drivers/virtio_blk.c`: modern MMIO transport only (refuses legacy), polled completion with a time bound, queue size 8, kernel-owned bounce page, only DT-named pages mapped Device | a working template. The transport code is **inside** the block driver; a second virtio driver means copying or extracting it |
| Interrupts | GICv2 and the architected timer exist (issue #5) | polling is enough for graphics; interrupts are optional |
| Input | none | no keyboard, no pointer, no focus model |
| Userspace | EL0 processes, syscalls, bounded IPC, private address spaces | a compositor could be a kernel module first and a user server later; not decided |
| Tests | `tests/qemu/*.sh` boot QEMU and read the serial log; `tests/kernel-host/*.c` build pure kernel logic on the host with ASan and UBSan | pure drawing maths belongs in a host-tested file; pixels belong in a QEMU test |

## 3. What QEMU offers (real output from this machine)

### Display backends **[verified]** (`qemu-system-aarch64 -display help`)

`none, gtk, sdl, egl-headless, curses, spice-app, dbus`. A VNC display is a separate option (`-display vnc=...` is in the usage text). For automated tests the useful
combination is `-display none` plus the monitor/QMP `screendump` (below); a person can add `-display gtk` or `sdl` to look at it.

### Display and input devices **[verified]** (`-M virt -device help`)

| Device | Bus | Kernel can use it today? |
|---|---|---|
| `ramfb` | System (no PCI) | needs fw_cfg (below) |
| `virtio-gpu-device` | virtio-mmio | yes, same transport as virtio-blk |
| `virtio-gpu-pci`, `virtio-gpu-gl-device/-pci`, `vhost-user-gpu*` | PCI / GL | PCI: no (no PCI driver); GL: out of scope |
| `VGA`, `bochs-display`, `secondary-vga`, `cirrus-vga`, `ati-vga` | PCI | **no**: the machine has a PCI host (`pci-host-ecam-generic` at `0x3f000000` with `highmem=off`) but the kernel has no PCI enumeration |
| `virtio-keyboard-device`, `virtio-mouse-device`, `virtio-tablet-device` | virtio-mmio | yes, later (stage G5) |
| `usb-kbd`, `usb-mouse`, `usb-tablet`, `qemu-xhci` | PCI USB | no |

Properties **[verified]**: `virtio-gpu-device` has `xres=1280`, `yres=800` defaults, `max_outputs=1`, `edid=on`, and the usual virtio feature switches (`packed`, `event_idx`, `indirect_desc`
and so on, all off or default); `ramfb` has only `x-migrate` and `use-legacy-x86-rom` (x86 only). `virtio-keyboard-device` and `virtio-tablet-device` have a `display` and `head` property.

### The real `virt` device tree with `-machine virt,highmem=off -cpu cortex-a53 -m 256M` **[verified]**

| Item | Value |
|---|---|
| RAM | one region at `0x40000000`, size `0x10000000` (256 MiB) |
| PL011 UART | `0x09000000` |
| GICv2 | `intc@8000000` |
| **fw_cfg** | `fw-cfg@9020000`, `reg = <0x9020000 0x18>`, `compatible = "qemu,fw-cfg-mmio"`, `dma-coherent` |
| **virtio-mmio** | **32** slots from `0x0a000000`, `0x200` apart, last at `0x0a003e00` |
| PCI | `pcie@10000000` node with `pci-host-ecam-generic`, ECAM `0x3f000000` |
| Framebuffer node | **none** (`grep -ciE 'framebuffer|simple-fb|display'` over the dumped tree is `0`, with ramfb, virtio-gpu, keyboard and tablet attached) |

Slot assignment **[verified]** (`info qtree`): devices take virtio-mmio slots from the **top down, in the order they are created**: with `-device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device`, the GPU got slot 31, the keyboard 30 and the tablet 29.
So a driver must scan every slot and match the **DeviceID**, as the block driver does; it must never assume an address.

### Headless pixel capture **[verified]**

The monitor command `screendump filename [-f format] [device [head]]` exists and supports `png` and `ppm`. With `-display none` and **no guest software**, both `ramfb` and
`virtio-gpu-device` produced a **640x480** P6 PPM (921,615 bytes) placeholder. So a test can capture the screen without any window, and a changed size or pixel content proves the guest set up a display.
QMP (`-qmp`) is also available; with only python3 and nc on the WSL side, a stdlib python script can drive QMP without `socat`.

### What the QEMU binary itself confirms **[verified]**

`strings` on `/usr/bin/qemu-system-aarch64` finds the fw_cfg file name **`etc/ramfb`** and the messages `ramfb device requires fw_cfg with DMA`, `ramfb_setup`, `ramfb_display_update`.
So ramfb is configured through a fw_cfg file and the **DMA** interface is mandatory.

## 4. The candidates

### A. `ramfb` over fw_cfg

- **Mechanism [spec, recalled]:** fw_cfg MMIO has a data register at `+0x00`, a 16-bit selector at `+0x08` and a 64-bit DMA address register at `+0x10` (the `0x18`-byte window in the tree matches). Read the file directory (selector `0x0019`: a big-endian count, then 64-byte entries `{u32 size, u16 select, u16 reserved, char name[56]}`), find `etc/ramfb`, and write one big-endian config struct `{u64 addr, u32 fourcc, u32 flags, u32 width, u32 height, u32 stride}` with a DMA write. The guest picks the memory; QEMU then reads it on its own refresh timer. XRGB8888 is the fourcc `XR24`.
- **Needs from the kernel:** a small fw_cfg driver, `vmm_register_mmio(0x09020000, ...)` plus a Device mapping, **one physically contiguous** framebuffer, and (preferably) a `fdt_fwcfg()` parse of the `qemu,fw-cfg-mmio` node. About 150 lines.
- **Present model:** none. There is no flush or damage call; pixels written to RAM appear at QEMU's next refresh. Fine for bring-up, but tearing is possible and there is no damage hint.
- **Fidelity:** QEMU-specific. It does not exist on real hardware; it is a bring-up convenience, not an architecture.

### B. `virtio-gpu-device` (virtio-mmio)

- **Mechanism [spec, recalled]:** virtio device ID `16`, a control queue. The 2D flow is `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D(id, format, w, h)`, `RESOURCE_ATTACH_BACKING(id, entries)`, `SET_SCANOUT`, then per frame `TRANSFER_TO_HOST_2D(rect)` and `RESOURCE_FLUSH(rect)`. Commands are `0x0100` to `0x0106`; common formats are `B8G8R8X8_UNORM = 2` and `X8R8G8B8_UNORM = 4`.
- **Needs from the kernel:** the virtio-mmio transport and split-queue code (already written for the block driver but not reusable as it stands), a control-queue request/response path with buffers larger than the block bounce page (`ATTACH_BACKING` carries a 16-byte entry per memory run), and a backing store. Scatter-gather means the backing need **not** be contiguous, but the CPU wants a contiguous view, so a contiguous region still simplifies drawing. About 350 to 500 lines plus an extracted shared transport.
- **Present model:** explicit and rectangle based (`TRANSFER_TO_HOST_2D` plus `RESOURCE_FLUSH`). It maps directly onto a compositor with damage rectangles, and onto resize and EDID later.
- **Fidelity:** the standard paravirtual GPU model; much closer to what a real driver does than ramfb.

### Comparison

| Criterion | ramfb | virtio-gpu-device |
|---|---|---|
| New kernel code (estimate, unverified) | about 150 lines, isolated | about 350 to 500 lines, plus extracting the shared virtio transport (touches the block driver and its tests) |
| Contiguous memory | required | optional (scatter list) |
| Present / damage | none | explicit rectangles |
| Resolution | whatever the guest declares | host default 1280x800, EDID, resizable in principle |
| Same transport as the existing driver | no (fw_cfg) | yes |
| Pairs with input | independent | pairs naturally with `virtio-input` over the same transport |
| Headless test | `screendump` | `screendump` |
| Risk of a wrong first step | low | medium (more protocol before the first pixel) |

### Rejected

PCI devices (`VGA`, `bochs-display`, `virtio-gpu-pci`, ...): they need PCI ECAM enumeration, BAR allocation and probably MSI or INTx routing, none of which exist, and the work would not carry over to the rest of the plan. OpenGL (`virtio-gpu-gl`): out of scope for Tier 0 Clear. `simple-framebuffer` from the device tree: the machine does not provide one.

## 5. Recommendation

**G2 uses `ramfb`.** It is the smallest correct step to the issue's first acceptance test ("Framebuffer renders in QEMU"): isolated (no change to the block driver or its tests), fully verifiable with `screendump`, and it fixes the contiguous-memory and test-harness questions that every later stage needs anyway.

**Behind a small surface interface from day one**, so the backend can change without touching the renderer:

```
struct gfx_surface { uint8_t *pixels; uint32_t width, height, stride_bytes; uint32_t format; };
int  gfx_init(void);                       /* 0 or a negative error; finds and starts a backend */
void gfx_present(struct gfx_rect damage);  /* no-op for ramfb; TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for virtio-gpu */
```

**A virtio-gpu backend follows as its own stage**, when explicit presentation, damage rectangles and virtio-input are needed (G5). **What would change this recommendation:** if the fw_cfg DMA path proves troublesome in G2, or if extracting the virtio transport turns out to be cheap, go to virtio-gpu first and skip ramfb.

## 6. G2 design (as proposed in G1; what was built is section 10)

| Question | Proposal |
|---|---|
| Resolution and format | fixed **1024x768, XRGB8888** (4 bytes per pixel). Size arithmetic: 1024x768x4 = 3,145,728 bytes = **768 pages** (3 MiB); 1280x800x4 = 4,096,000 = 1000 pages; 640x480x4 = 1,228,800 = 300 pages |
| Memory | claim a fixed region at the **top of RAM** with `pmm_reserve_range()` early in boot (before anything is allocated), rounded to 2 MiB. 256 MiB of RAM is 65,536 pages, so 768 is about 1.2 percent. It is already mapped Normal WB read/write by the identity map; nothing in the MMU changes |
| Checks | width and height non-zero and bounded, `stride >= width*4`, `stride*height` overflow-checked and within the region; every draw primitive clips to the surface and returns an error rather than writing out of bounds |
| fw_cfg access | register the window, map it Device, read the directory, find `etc/ramfb`, one DMA write of the config. Errors (no fw_cfg, no `etc/ramfb`, DMA error bit, bad size) leave graphics **disabled with a clear serial message**; the OS still boots on the UART |
| Pure logic | rectangle intersection and clipping, fill spans and the size arithmetic in a file that builds on the host and is tested in `tests/kernel-host` with ASan and UBSan (the same pattern as `policy_test.c`) |
| First pixels | `gfx_clear(colour)` and `gfx_fill_rect(rect, colour)` with clipping |
| Evidence | `tests/qemu/gfx-tests.sh`: boot with `-device ramfb -display none -qmp unix:...`; wait for the serial line `gfx: framebuffer 1024x768`; send QMP `screendump` as `png`/`ppm`; a python3 script checks the **dimensions are 1024x768 (not the 640x480 placeholder)** and samples pixels (a known fill rectangle, its exact edges, an untouched corner). A `-display gtk` run is a manual look, not evidence |
| Negative tests | no `-device ramfb` (graphics must report disabled and the OS must still boot); rectangle partly or wholly outside the surface; zero-size and overflowing rectangles |

## 7. Stages

| Stage | Deliverable | Issue #7 acceptance it serves |
|---|---|---|
| **G1** (this document) | research and plan; no code | none yet |
| **G2** | fw_cfg, ramfb, `gfx_surface`, clear and clipped fill; `screendump` evidence; host and QEMU tests | "Framebuffer renders in QEMU" |
| **G3** | rounded rectangle and 1 px border, integer-only where possible (no libm), exact coverage rules, host-tested against a reference | "Rounded rect, border ... work" |
| **G4** | text: a bitmap font with a **clear licence** (own or permissively licensed, decided before use), measure and wrap, clipping, a size ladder; Latin only | "... text, clipping work" |
| **G5** | `virtio-input` keyboard and tablet drivers, an event queue, hit testing and a focus ring; optionally the virtio-gpu backend with real damage presentation | pointer/keyboard focus |
| **G6** | window composition and the Tier 0 Clear interface: layers in the prototype's z-order, opaque only, a documented API; a decision on kernel module vs EL0 server | "Tier 0 Clear is a complete supported interface" |

The Tier 0 pipeline must **not** include alpha blending, blur or gradients; those are Tier 1 and later and need measurements first (`aurora-glass/open-design-handoff/native-compositor-mapping.md`).

## 8. Risks and unknowns (do not hide these)

1. **Every protocol detail marked [spec, recalled] is unverified**: the fw_cfg register offsets and DMA struct, the directory format, the ramfb config layout and fourcc, and the virtio-gpu command numbers and formats. G2 must confirm each by observing real behaviour, and the plan's estimates of code size are guesses.
2. **Cache coherence is not tested.** The framebuffer is written through Normal write-back memory. QEMU's TCG reads guest RAM directly and does not model caches, so no maintenance should be needed *here*; real hardware, or an accelerator such as KVM, would need a clean to the point of coherency (`dc cvac`) or a Normal non-cacheable attribute, and MAIR has none. The `fw-cfg` node in the device tree carries `dma-coherent`; that describes fw_cfg's own DMA path and says nothing about whether the guest's pixel writes are visible.
3. **ramfb has no present step**, so a frame can be sampled half-drawn. Tests must draw, then wait for QEMU's refresh, or use virtio-gpu for deterministic presentation. G2 should measure how long a `screendump` needs to see an update before fixing any wait in a test.
4. **A contiguous 3 MiB claim is fine at 256 MiB of RAM but is a design constraint**: a larger resolution or a second buffer (double buffering) needs more, and the PMM has no contiguous allocator if the claim is ever made late.
5. **No text stack at all**: no font, no layout, and Hindi and Gujarati would need script shaping that this plan does not cover. The browser prototype only proves string plumbing.
6. **No input stack**, no IRQ-driven drivers beyond the timer and GIC, no EL0 interface to the display.
7. **Performance is unmeasured.** A CPU fill of 3 MiB per frame at this clock rate on TCG has not been timed; G2 should record a real number, labelled as QEMU/TCG, before anything depends on it.
8. **The `virt` machine may change between QEMU versions.** Slot assignment, the default 640x480 placeholder and the fw_cfg window were observed on QEMU 11.1.0 only.

## 9. Open decisions for the owner (resolved by the owner before G2: 1024x768 XRGB8888; Noto Sans Mono under the OFL 1.1 for G4, licence to be verified before any asset is added; kernel-side components first, an EL0 compositor later; no virtio transport refactor until virtio-gpu or virtio-input needs it; ramfb is the bring-up display and a QEMU-only Tier 0 fallback)

1. Resolution for G2 (proposed 1024x768) and whether to support more than one.
2. The font source and licence for G4.
3. Whether the compositor lives in the kernel at first or goes straight to an EL0 server (the process and IPC model exists).
4. Whether to extract a shared virtio transport soon (it helps `virtio-gpu` and `virtio-input`) or only when G5 needs it.
5. Keep ramfb only as a bring-up path, or keep it supported long-term.

## 10. G2 as implemented (QEMU `virt` + ramfb only)

**Status: implemented and verified in QEMU on QEMU 11.1.0 under TCG (`-cpu cortex-a53`). Nothing here has run on real hardware, KVM or any accelerator.** The kernel finds fw_cfg, configures ramfb with a DMA write, draws a diagnostic pattern itself, and QEMU's own `screendump` is parsed to prove the pixels. Stage G3 is section 11 and G4 section 12; G5 onward is not done.

### What exists

| Piece | File | Notes |
|---|---|---|
| Device-tree discovery | `kernel/mm/fdt.c` `fdt_fwcfg_mmio()` | the walker used for virtio-mmio now takes a compatible string; fw_cfg is `qemu,fw-cfg-mmio`; zero size and address wrap are rejected |
| Pure protocol helpers | `kernel/drivers/fwcfg_proto.c`, `kernel/include/fwcfg.h` | big-endian helpers, directory walk through an ops table, DMA descriptor (a plain 16-byte array, no host byte order assumed), ramfb configuration builder; host-tested with a fake device |
| MMIO driver | `kernel/drivers/fwcfg.c` | `fwcfg_probe` (read-only, runs before the MMU), `fwcfg_map`, `fwcfg_lookup`, `fwcfg_dma_write` with a bounded wait |
| Renderer | `kernel/gfx/gfx_draw.c`, `kernel/include/gfx.h` | `struct gfx_surface` (pixels, size_bytes, width, height, stride_bytes, format), clear, pixel, clipped fill, h/v lines, the test pattern; every size computed in 64 bits; padding between rows is never written |
| Placement | `kernel/gfx/gfx_region.c` | highest contiguous run of free pages in RAM, found through a callback so it can be host-tested |
| Backend | `kernel/gfx/gfx.c` | `gfx_reserve` (early), `gfx_init`, `gfx_get_surface`, `gfx_present`, `gfx_shutdown` |
| Boot wiring | `kernel/kernel.c` | `gfx_reserve` after the device-tree reservations and before `vmm_init`; `gfx_init` and the pattern after the memory self-tests |

### Boot sequence and memory

1. `gfx_reserve(fdt)` runs **before** `vmm_init` because the allocator has no contiguous allocation and page-table setup consumes pages. It reads fw_cfg (safe with the MMU off), requires the signature `QEMU`, the DMA feature bit and `etc/ramfb` in the directory, then asks `gfx_region_find` for the highest 3 MiB run of pages that are neither reserved nor allocated, and reserves it with `pmm_reserve_range`.
2. **No display means nothing is reserved**, and the boot prints `gfx: disabled (<reason>)` and carries on.
3. After the MMU is on, `gfx_init` registers the fw_cfg page with the mapping policy and maps it as Device memory, clears the framebuffer to black, and sends the 28-byte ramfb configuration with one fw_cfg DMA write. Any failure disables graphics; the region stays reserved (the allocator cannot un-reserve).
4. The framebuffer is ordinary Normal write-back RAM, already mapped read/write and never executable by the identity map. **It is never mapped into an EL0 address space.**
5. With 128, 256 and 512 MiB of RAM the framebuffer is the top 3 MiB: `0x47d00000`, `0x4fd00000`, `0x5fd00000`. A reservation conflict (device-tree reservation, kernel image) moves it down to the next free run; if none exists graphics is disabled.
6. Single CPU, one framebuffer, one caller at a time; there is no locking.

Side effect on an existing test: `selftest.c` assumed the last RAM page is free (`never-allocated page`). It now searches downward for a page that is genuinely neither allocated nor reserved; the assertion itself is unchanged.

### Protocol facts: what was verified, and against what

The G1 `[spec, recalled]` items are now checked against the QEMU documentation (https://www.qemu.org/docs/master/specs/fw_cfg.html), QEMU's `hw/display/ramfb.c` (current master), and the behaviour of the installed QEMU 11.1.0.

| Item | Value | Source / evidence |
|---|---|---|
| MMIO registers | data `+0x00`, selector `+0x08` (16-bit), DMA address `+0x10` (64-bit); window `0x18` | fw_cfg spec; the device-tree node reports `reg = <0x9020000 0x18>` |
| Byte order | selector and DMA register **big-endian**; data register string-preserving (the driver reads bytes); feature bitmap (`0x0001`) **little-endian** | fw_cfg spec; a host test pins the little-endian reading |
| Signature | key `0x0000` reads `QEMU`; the DMA register reads back `0x51454d5520434647` ("QEMU CFG") | spec; checked at run time by `fwcfg_probe` |
| Features | bit 1 = DMA | spec |
| Directory | key `0x0019`: big-endian count, then 64-byte entries `{be32 size, be16 select, be16 reserved, char name[56]}` | spec; the run finds `etc/ramfb` this way |
| DMA descriptor | `{be32 control, be32 length, be64 address}`; control bits: error 1, read 2, skip 4, select 8, write 16; selector in the upper 16 bits; completion = control reads 0, error = bit 0 | spec; the DMA write is accepted by QEMU 11.1.0 |
| ramfb file | `etc/ramfb`, 28 bytes, **write** with DMA: `{be64 addr, be32 fourcc, be32 flags, be32 width, be32 height, be32 stride}` | `ramfb.c` (`RAMFBCfg`, `QEMU_PACKED`); size 28 |
| Pixel format | fourcc `XR24` = `0x34325258` = DRM `XRGB8888`; in memory the bytes are B, G, R, X (a little-endian `0x00RRGGBB`) | `ramfb.c` converts the fourcc to a pixman format; verified by pure red, green and blue swatches reading back as red, green and blue in the screenshot, so no channel swap was needed |
| Limits QEMU enforces | width and height at least 16; a zero stride means tight; **QEMU does not check that a non-zero stride is at least `width*4`, so the driver does**; a framebuffer that does not fit in guest RAM makes QEMU keep its placeholder | `ramfb.c`; a mutation with a wrong stride produced the 640x480 placeholder |

Constants are written independently against the documented interface; no QEMU code was copied.

### Evidence (tests/qemu/gfx-tests.sh, 45 checks, all passing)

- **Pixels:** `tests/qemu/gfx_check.py` drives QMP `screendump` (headless `-display none`, private unix socket) and parses the PPM with the standard library. It requires 1024x768 (the 640x480 placeholder QEMU shows before any guest configures it fails the size check), **52 exact pixel assertions** (background, panel edges, both rules, three primary swatches, four corner markers and their boundaries, two clipped fills that start off-screen), **exact pixel counts for all nine colours**, and that the image contains no other colour. 67 checks per run.
- **Timing:** QEMU samples guest RAM on its own refresh. In the runs the first screendump after the serial line (2 to 4 s after QEMU started) already matched. The checker still polls on image content with a bounded deadline rather than sleeping.
- **RAM sizes:** 128, 256 and 512 MiB each verified, then re-verified **after the whole boot** (all EL0 and filesystem self-tests) so nothing overwrote the framebuffer.
- **No display:** a normal boot without `-device ramfb` reports `gfx: disabled (no ramfb display: file not in the fw_cfg directory)`, reserves nothing (exactly 768 fewer reserved pages than with ramfb), and passes memory, interrupt and userspace tests. A boot with only `virtio-gpu-device` also disables graphics without a crash. ramfb together with a virtio-blk disk works.
- **Host tests (ASan/UBSan, `tests/kernel-host/run.sh`):** `fwcfg_test` 54 checks (directory bounds, duplicate and unterminated names, every endianness conversion, DMA status decoding, ramfb limits) and `gfx_test` 101 checks (size arithmetic including 8192x8192 and `INT32_MIN/MAX` rectangles, clipping, non-tight strides, guard canaries before and after every buffer, placement in fragmented RAM, the same pixel expectations as the screenshot). Plus 10 checks for the fw_cfg device-tree walker.
- **Mutation checks** (a deliberately broken kernel must fail the screenshot test): red/blue swapped in `gfx_rgb`, wrong ramfb stride, wrong ramfb height, configuration never sent, a fill one pixel short, negative-origin clipping dropped. All were caught.

### Cache coherence and memory ordering (not proven on hardware)

- The device tree marks fw_cfg `dma-coherent`: the device reads the DMA descriptor from, and writes its result to, the same memory the CPU sees. The driver therefore does no cache maintenance for the descriptor, only a `dsb sy` between writing it and writing the DMA address register, and again while polling.
- The framebuffer is Normal write-back memory and there is no non-cacheable attribute in MAIR. QEMU TCG does not model caches, so **a pass under TCG does not show that a cache-incoherent scan-out engine would see the pixels.** Anything that is not coherent (real display hardware, some accelerated setups) would need `dc cvac` of each damaged range in `gfx_present`, or a non-cacheable mapping. That is not implemented and not claimed.
- `gfx_present` is a barrier only: ramfb has no present step and tearing is possible.

### Known limits of G2

QEMU only. Fixed 1024x768 XRGB8888 (the surface API carries width, height, stride and format, but the backend only configures this size). No double buffering, no damage handling, no input, no text. Performance is unmeasured. The diagnostic pattern is drawn at every boot of a display-attached kernel; it is a test image, not a desktop.

### Status of the later stages

| Stage | State |
|---|---|
| G1 | done |
| G2 | **done on QEMU**, as above |
| G3 | **done on QEMU**, section 11 |
| G4 | **done on QEMU for printable ASCII**, section 12 |
| G5 | **done on QEMU**, section 13 |
| G6 | **layered scene done on QEMU**, section 13; application windows and an EL0 compositor service not started |

## 11. G3 as implemented: rounded rectangles, borders and exact clipping (QEMU/TCG only)

**Status: implemented and verified on QEMU. Opaque pixels only: no anti-aliasing, alpha, blur or gradients (those are Tier 1 and later).**

API (`kernel/include/gfx.h`, code in `kernel/gfx/gfx_shape.c`): `gfx_fill_round_rect(surface, rect, radius, colour)`, `gfx_stroke_round_rect(surface, rect, radius, border, colour)`, `gfx_round_clamp_radius`. Everything is integer arithmetic (int64), no libm and no floating point.

### The coverage rule (exact, documented in the source)

A pixel belongs to a rounded rectangle iff its **centre** (x + 1/2, y + 1/2) is inside the rectangle and, when it lies in a corner quadrant, no farther than the radius from that corner's circle centre. The corner circle centres sit at (rx + r, ry + r), (rx + w - r, ry + r), (rx + r, ry + h - r), (rx + w - r, ry + h - r) in pixel-edge coordinates. In doubled coordinates: `dx = 2x + 1 - 2cx`, `dy = 2y + 1 - 2cy`, inside iff `dx*dx + dy*dy <= (2r)^2`. The renderer turns this into an exact span per row with an integer square root, so each row costs one root, not a per-pixel test.

- **Radius** is clamped to `[0, min(w, h) / 2]`. Radius 0 is a plain rectangle. A square with radius w/2 is a circle; a wide rectangle with radius h/2 is a pill. Radius 1 trims nothing (every pixel centre of a 2x2 or larger shape is within 1 of its corner centre).
- **Border** of width b is drawn **inside** the rectangle: the outer shape minus the shape inset by b on every side with radius `max(r - b, 0)`. The corner centres coincide, so the border has even thickness and border + inner fill equals the outer fill exactly, with no gap and no overlap. A border at least half as thick as the shape fills it completely. `border <= 0` draws nothing.
- **Clipping** clips each row span to the surface; nothing outside is written and a shape that starts far off-screen costs only its visible rows. Padding between rows is never written. Non-positive sizes draw nothing. Invalid surfaces are refused through `gfx_surface_check` before any write.
- **Overflow:** the largest intermediate is `(2r)^2` with `r <= INT32_MAX / 2`, below 2^62; UBSan runs with `INT32_MIN/MAX` rectangles, radii and borders.

### Evidence

- `tests/kernel-host/gfx_shape_test.c`, **44 checks** under ASan/UBSan: hand-drawn ASCII art for 4x4 r2, 5x5 r2, 8x8 r4, a 10x5 pill and an 8x8 ring; **4,680 fills and 17,836 borders compared pixel by pixel against an independent per-pixel oracle** (the oracle tests each pixel centre with no square roots and no spans); 312 clipped placements (partly or wholly off-screen); three non-tight strides with padding checks and guard canaries; mirror symmetry in x and y; monotonic area in the radius; border + inner = outer; nesting; circle area within 1 percent of pi r^2; extreme values; invalid surfaces.
- `tests/qemu/gfx-tests.sh` (now **50 checks**) builds a test kernel with `AURORA_GFX_SHAPES=1` that draws the G3 image (`gfx_draw_shapes_pattern`: fills, a border-only shape, a pill, a circle, a fill with border, two shapes clipped at opposite screen corners, a clamped huge radius, 1x1 to 8x8 tiny shapes, a square-cornered border, a border thicker than half, a 1 px outline). `gfx_check.py --pattern shapes` re-renders the **whole 1024x768 image with its own independent per-pixel Python implementation** and requires the QEMU screenshot to match it **byte for byte**, plus 29 hand-derived anchor pixels. It is re-checked after the full boot.
- **Mutation checks** (the byte-exact comparison must fail): circle half-width one too small, floor instead of ceil on the left edge, non-concentric border radius, radius clamp one too large, wrong centre for the bottom corners, right border span off by one. All six were caught.
- Normal kernels do not contain the shapes image; the G2 screenshot expectations are unchanged.

### Limits

QEMU/TCG only. No anti-aliasing: edges are hard, which is the Tier 0 contract. The shapes are not yet connected to any UI element. Performance is unmeasured. Text (G4), input (G5) and window composition (G6) are not started.

## 12. G4 as implemented: native bitmap text (QEMU/TCG only, printable ASCII only)

**Status: implemented and verified on QEMU for printable ASCII in Noto Sans Mono. This is NOT multilingual text support: Hindi and Gujarati are not supported (they draw as boxes), there is no shaping, no bidirectional text, no combining marks, no input, no focus and no screen-reader or text-to-speech path.** Text is opaque 1-bit bitmaps (Tier 0).

### Font source and licence (verified before any asset was added)

Noto Sans Mono, release `NotoSansMono-v2.014` (2023-09-30) of https://github.com/notofonts/latin-greek-cyrillic, files `googlefonts/ttf/NotoSansMono-Regular.ttf` and `-Bold.ttf`, pinned by SHA-256 in `tools/fontgen/gen_font.py`. Licence: SIL Open Font License 1.1; `OFL.txt` is byte-identical to the file inside the release archive and to the repository file at the release tag; copyright "2022 The Noto Project Authors"; no Reserved Font Name declared. The generator also reads the licence strings embedded in the font (name IDs 13, 14) and refuses anything that does not say OFL 1.1. The TrueType files are not committed; `third_party/fonts/NotoSansMono/` holds `OFL.txt`, `AUTHORS.txt` and a provenance `README.md`. The generated `kernel/gfx/font_noto_mono.c` is a modified version of the font software (outlines rasterized to bitmaps) and says so, with the notices, in its header. No proprietary or system font was used. (A distribution-packaged copy of Noto Sans Mono was present on the build machine but was deliberately not used; the pinned upstream release is.)

### Atlas generation (`tools/fontgen/gen_font.py`, reproducible)

Python standard library only. It parses the TrueType tables itself (head, maxp, hhea, hmtx, cmap formats 4 and 12, loca, glyf with simple and translated composite glyphs; hinting is ignored), flattens the quadratic outlines, rasterizes with the non-zero winding rule at 8 sub-scanlines per pixel with exact horizontal span coverage, and keeps a pixel when at least half of it is covered. A glyph that would keep under 60 percent of its covered area (a thin stem straddling a pixel boundary) is re-thresholded as low as 25 percent so it cannot vanish; this was needed for `|` at 14 px, which a host test had found to be blank. Glyphs are laid on an integer advance (0.6 em rounded) and scaled horizontally by at most about 4 percent so they fill the cell pitch exactly. Output: 10 faces (regular 400 and bold 700 at 12, 14, 16, 18, 24 px), 14,287 bytes of bitmap. **Reproducibility checked:** two runs give identical bytes, and Windows Python 3.13 and WSL Python 3.14 both reproduce the committed file byte for byte; a font with a different hash is refused.

### Engine (`kernel/gfx/gfx_text.c`, API in `gfx.h`)

One UTF-8 code point is one cell. Strict decoding (overlong forms, surrogates and values above U+10FFFF are invalid; an invalid sequence consumes exactly one byte). U+0020 to U+007E draw their glyph; every other code point, including controls, DEL, `\r` and `\t`, accented Latin, Devanagari and Gujarati, draws a **visible hollow missing-glyph box** (documented size). `gfx_text_measure`, `gfx_text_wrap` (greedy; breaks at the last space run, else between cells; spaces at a break are dropped; `\n` honoured; at least one cell per line), `gfx_draw_text` (first line, clipped to the surface and an optional rectangle) and `gfx_draw_text_block` (wraps into a box, draws every line whose top is above the box bottom, clipped to the box). Line spacing is the face's `line_height`; the baseline is `y + ascent`. Input is bytes with an explicit length, capped at 1 MiB; a corrupt glyph table is skipped, never read out of bounds.

### Evidence

- `tests/kernel-host/gfx_text_test.c`, **109 checks** under ASan/UBSan: font data integrity (all ten faces, every glyph inside its bit array and its cell box, tight boxes, advance rule, provenance hashes, licence files), UTF-8 (21 hand cases plus 200,000 random byte strings: always progresses, never overruns, never yields a surrogate or a value above U+10FFFF), measuring, 21 hand-worked wrap cases plus invariants on 20,000 random texts (no line too wide, no trailing space, every visible character survives in order), glyph pixels against the table, **2,100 clipped renderings equal to the crop of an unclipped rendering**, strides with padding checks and guard canaries, INT32 extremes, corrupt font data, text blocks (no pixel outside the box).
- `tests/qemu/gfx-tests.sh` (now **55 checks**): a test kernel built with `AURORA_GFX_TEXT=1` draws the G4 image (every size and weight, the full ASCII range, missing glyphs, wrapped blocks in wide, narrow and short boxes, clipping at all four screen edges and to a rectangle); `gfx_check.py --pattern text` renders it again with its own UTF-8 decoding, its own paragraph-style word wrap and its own blitter over the glyph data parsed from the C file, and requires the screenshot to match **byte for byte**, also after the whole boot. The reference image was also looked at by eye: all sizes and both weights are legible; boxes, wrapping and clipping are as designed.
- **Mutation checks** (the screenshot test must fail): baseline one pixel low, glyph x offset flipped, one extra column per line, UTF-8 continuation check disabled, clip one pixel too wide, narrower missing-glyph box, line height off by one, spaces at the edge not dropped. All eight were caught.

### Limits (not hidden)

ASCII only. 1-bit glyphs at five sizes: hard-edged, hinting-free, so small text is rougher than anti-aliased text would be. Indic scripts need shaping, which is not planned yet. No kerning or proportional layout (monospace only). No text input or editing, no selection, no accessibility interface. QEMU/TCG only; performance unmeasured.

## 13. G5, G6 and the first native shell as implemented (QEMU/TCG only)

**Input (PRs #60, #61).** `kernel/drivers/virtio_input.c` drives virtio-input (device ID 18, modern virtio-mmio only) by probing every device-tree slot; capabilities come from the device configuration space (key bitmap, tablet axis ranges: QEMU 11.1 reports 0..32767). The eventq is filled with 16 device-writable 8-byte buffers and polled; used-ring ids are range- and ownership-checked and a device that breaks the ring protocol is reset. `kernel/drivers/virtio_mmio.c` holds the one thing blk and input share (declare and map the device tree's virtio window once); queues stay in each driver. `kernel/input/input_core.c` is pure: modifiers, a documented US layout (no caps lock), 64-bit pointer scaling, a bounded queue. `kernel/input/ui_focus.c` is pure: z-ordered hit testing with rounded shapes, absorbing covers, Tab/arrow focus, Enter/Space activation (repeat ignored), press-and-release click rule, a bounded modal stack with focus restore.

**Scene and shell (PRs #62, #64, #65, #66).** `kernel/ui/ui_scene.c` draws opaque widgets in (layer, order); `kernel/ui/shell.c` (pure, host-tested) owns screens, a bounded navigation history, layout and dispatch for Home, Dock, Spaces, Index (`index_model.c`, an independent port of the browser ranking model), About, the Glass Corner sheet and the Horizon rail; `kernel/ui/shell_run.c` is the kernel loop (`wfi` between 100 Hz ticks, a `shell:` log for tests). The real kernel boots into it when a display exists; test and demo builds keep their own screens. The UI is not an authorisation mechanism and performs no privileged action. A future EL0 compositor service keeps the scene/focus API and replaces the kernel-side shell; applications will never receive the framebuffer, the scene array or the focus context.

**Evidence.** Host (ASan/UBSan): input 45, ui-focus 62, ui-scene 25, shell 138, index-model 49. QEMU: `input-tests.sh` 17, `ui-tests.sh` 8, `home-tests.sh` 15 (with `home_check.py` 78 checks of real QMP key and pointer events against guest log lines and screendump pixels), `gfx-tests.sh` 55. Host tests found real defects (non-interactive surfaces being activatable; focus not restored after a pointer-opened sheet; stale layout after typing) that were fixed, not worked around.

**Measured (see `native-graphics-performance.md`).** On QEMU/TCG (i7-1355U, QEMU 11.1.0): 1024x768 clear about 2-5 ms; a 100-control full-frame redraw about 7-12 ms median; a one-widget damaged redraw about 0.05-0.1 ms; run-to-run variation about 2x. Nothing about real hardware.

**Not done.** Application windows, an EL0 UI service, non-ASCII text, text editing beyond end-typing and Backspace, a user-reachable way to change text scale or contrast (see section 14), persistence, a real clock, notifications, interrupt-driven input (polled), the statusq (LEDs), caps lock, real-hardware cache coherence.

## 14. Text scaling and high contrast (QEMU/TCG, Tier 0)

**Scaling.** `gfx_draw_text_mag` magnifies the 1-bit glyphs by an exact integer (1 to 4), so 200 percent is a true doubling: the 14, 16, 18 and 24 px roles become 28, 32, 36 and 48 px. Every text role maps a percentage to the nearest size of a fixed ladder (12 14 16 18 24 native, 24 28 32 36 48 magnified); values between snap to a real size and text never goes below 12 px, so 75 percent shrinks only the larger roles. The scale range is 75 to 200 (the settings registry range); other values are refused.

**Reflow.** 100 percent keeps the reference layout exactly (the earlier tests are untouched). Other scales use a flow layout in `kernel/ui/shell.c`: labels and buttons wrap, buttons grow to hold their label and never drop below the 48 px reference touch size, the header moves the status text to its own row when it does not fit, the dock widens, and the Index results become a window that always contains the selected row ("Showing 1-4 of 8"). At the largest scales (about 190 percent and up) the optional Home caption "System" is omitted rather than clipped. Nothing is placed outside the region between the status rail and the dock.

**High contrast.** A runtime preference (`shell_set_contrast`): opaque white surfaces, black text (at least 12.6:1 measured), 2 px borders on enabled controls and 1 px on disabled ones, a marker bar inside the active control in addition to its inverted colours, a `0x0000E0` focus ring (3 px), a black scrim. State is never colour-only. It applies to every shell screen, the About modal and the Glass Corner sheet.

**Contrast fix (Cycle 21).** The native Clear accent is now `0x2A68E8` (white on it 4.95:1; it was `0x3478F6` at 4.07:1) and the Horizon/aura status rail is `0x00838C` (4.54:1 against the white surface, 4.07:1 against the canvas; it was `0x00AEB8` at 2.71 / 2.43). Measured by `shell_scale_test.c` over 2,772 layouts: worst standard text 4.95:1, high contrast 12.63:1. The `ctl` enabled-control border was done later (see the Cycle 22 note below); the browser prototype tokens are intentionally unchanged.

**Not done.** High contrast does not persist (no registry setting exists for it; it is session-only and the Settings screen says so). Text size is changed by the Settings screen (section 15). The scale QEMU tests still select the appearance with the test-only build flags `AURORA_SHELL_TEXT_PCT` and `AURORA_SHELL_CONTRAST`; the shipped kernel starts at the stored text size, or 100 percent, standard contrast. No screen reader. No claim about real hardware.

**Evidence.** Host (ASan/UBSan) `shell_scale_test.c`, 40 checks including an audit of 2,772 layouts (every scale 75 to 200, both palettes, eleven states) against an independent word-wrap oracle and an independent WCAG contrast calculation; the earlier shell (138), scene (25), focus (62) and input (45) tests pass unchanged. QEMU `tests/qemu/scale-tests.sh`: four boots (200, 200 high contrast, 75, 150 high contrast), each 36 or 38 checks driven by real QMP key and pointer events, with every screendump pixel required to belong to the mode's palette and a 2 px band around every control required to be clean of text.

## 15. The native Settings screen (QEMU/TCG, Tier 0)

A Settings screen (Home tile and Index, both keyboard and pointer) lists the six registry settings and a session-only high-contrast switch, each with a value button (Enter or a click steps to the next documented value) and a Reset button, a status line that reports exactly what the registry answered, and scroll controls when the list does not fit (at least two rows are visible at every text size). Rows are labelled **works now** (Text size, High contrast), **stored only** (Theme, Reduced motion, Focus mode, Aura mode, Notifications: no dark theme, nothing animates, no notification source, Aura does not exist) and the screen states that brightness, sound, Wi-Fi and battery are not supported because there are no drivers. Details, the principal and the trust boundary are in `docs/kernel/settings.md`.

**Behaviour that matters.** A change is saved first and applied only on success. A failed save, a refusal (the shell has no system permission, so Aura mode is refused by the registry) and a newer-version record change nothing and are shown as such. With no usable store, changes last for the session and the screen says so. The saved text size is applied at boot.

**Evidence.** Host `shell_settings_test.c` (42 checks; a fake registry that fails, denies and reports a newer version; session-only mode; scrolling at 200 percent; 6000 random actions) and QEMU `tests/qemu/settings-ui-tests.sh` (five boots, 8 checks plus 41 scenario checks with real injected keyboard events: no disk, save, restart and reload on the same disk, read-only disk, destroyed filesystem). The Settings screen is also part of the 2,772-layout audit.

**Not done.** Simple/advanced views, search inside Settings, any Aura or permission UI, persistence of high contrast, a corrupt or newer-version settings record exercised through the UI on QEMU (only the whole-filesystem failure is; the record cases are covered by `settings-tests.sh` and by the fake registry), an EL0 Settings client.

## Appendix: commands that produced the **[verified]** facts

All on WSL `kali-linux`, QEMU 11.1.0:

```
qemu-system-aarch64 -display help
qemu-system-aarch64 -M virt -device help
qemu-system-aarch64 -M virt -device ramfb,help            # and virtio-gpu-device,help / virtio-keyboard-device,help / virtio-tablet-device,help
qemu-system-aarch64 -machine virt,highmem=off,dumpdtb=/tmp/virt-gfx.dtb -cpu cortex-a53 -smp 1 -m 256M -nographic -display none \
    -device ramfb -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device ; dtc -I dtb -O dts /tmp/virt-gfx.dtb
printf 'info qtree\nquit\n' | qemu-system-aarch64 ... -monitor stdio -global virtio-mmio.force-legacy=false -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device
printf 'screendump /tmp/x.ppm\nquit\n' | qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -m 64M -S -nographic -display none -serial none -monitor stdio -device ramfb   # and virtio-gpu-device
strings /usr/bin/qemu-system-aarch64 | grep -E 'etc/ramfb|ramfb device requires'
```

## 15. The scheduler-aware shell loop (implemented; blocker 1 resolved)

The shell loop no longer starves EL0 processes. Each iteration polls input, applies it, redraws if needed, reaps finished processes and then, **if any process is READY**, calls `sched_run_slice(6)`: a bounded run of at most 6 scheduler ticks (1 ms each while it runs). The existing timer watchdog returns control to the kernel, saving the running process as READY, and the loop goes straight back to the input. With nothing READY the loop sleeps in `wfi` on the 100 Hz tick as before, so a blocked client costs no CPU and an empty system idles.

What had to be understood first (all in `kernel/proc/sched.c`): a context switch is a frame copy followed by the normal exception return; `sched_run` parks the kernel's own frame, runs until nothing is runnable or a tick limit, restores the kernel frame and **stops the timer**. Two properties of `sched_run` would have broken repeated slices, so a separate entry (`SVC #0x5c4f`) was added and `sched_run` itself is unchanged: it **resets the tick count** each call (`SYS_TICKS` would run backwards) and starts the round-robin search at pid 1 each call (two CPU-bound clients would starve the second). `sched_run_slice` keeps both across calls and the shell restarts its own timer before sleeping.

Finished and faulted processes are reaped by the shell loop (`proc_destroy`, kernel code only; no syscall exists for it). Process faults were already contained by the scheduler; they now also leave the shell running.

**Evidence** (`tests/qemu/shell-sched-tests.sh`, six sequential QEMU boots, 45 checks, real injected input, screendump polling; EL0 execution proven by guest `[user pN]` lines printed after the shell became interactive): no clients (baseline), two CPU-bound clients (both progress, preempt each other; Tab-to-screen latency median 31 ms, max 156 ms versus 32 and 42 ms with no client), a yielder plus a client blocked on IPC (the blocked one runs no more; yielder keeps going), a normal exit (code 7) and an EL0 fault (contained, both reaped, shell responsive), 25 clients created and destroyed in sequence (all reaped, none failed), and a display with no input devices (clients still run).

**Not done.** No client talks to the compositor yet. Memory pressure is exercised only by the process table filling during churn. Measurements are QEMU/TCG on one PC and say nothing about real hardware. A no-display boot never reaches the shell loop and therefore never runs clients (unchanged).

## 16. The first EL0 UI clients (phase P1, kernel glue; QEMU/TCG only)

EL0 processes can now put a surface on the real shell. `SYS_UI` (number 41; 10..40 stay `-ENOSYS`) has two operations: **send** one bounded protocol message (`ui_proto.h`) and **poll** one event. `kernel/ui/ui_sys.c` takes the caller's pid, generation and principal from the process table (never from the message), checks the **UI capability** (granted only by kernel code, held by nobody by default), runs the pure protocol module, auto-places a new surface (the compositor chooses the position, inside the content region, clear of the header and dock) and queues events per client (four deep, oldest dropped, a new instance never sees the previous one's events). The shell draws each placed surface in the content band: **chrome above it, in a colour only the compositor uses, titled with the kernel's name for the process, not anything the client says**, a body, and the client's committed widgets as inert labels and panels. Pointer presses on a surface give it focus (the compositor moves focus, on the user's press); keys go only to the focused surface's owner; Tab and Escape always belong to the shell; an open modal or sheet withholds all input from clients; surfaces vanish when the shell reaps the process.

**Evidence** (`tests/qemu/ui-client-tests.sh`, two boots, real injected pointer and key events, screendump pixels and guest `[user pN]` lines): see the PR for the exact results of the CI run. Clients: two demo clients, a hostile client (wrong version, compositor-only type, truncated, oversized, bad widget index, out-of-bounds and zero-size rectangles, bad size class, non-ASCII title, guessed foreign handles, quota, bad pointers, send of 65 bytes, unknown operation, then a flood of invalid messages and focus requests), a client that faults after drawing, and a client without the capability.

**Not done.** Surfaces are text labels and panels only (no pixel buffers, no shared memory). No pointer press-grab. The client's title is stored but not drawn. The framebuffer is never reachable from a process; that rests on the existing mapping tests and on the interface having no way to name an address, not on a new test. No application model or launcher yet: the clients are started by test-only kernel code. The compositor is still kernel code (P3 is not started).

**Enabled-control edge contrast (Cycle 22).** The border of an ENABLED button is now `ctl` = `0x6B7C93` in the standard palette (black in high contrast); it was the decorative card border `0xC9D4E3` at 1.50:1. Measured over every scale (75-200%), contrast mode and state layout by `tests/kernel-host/shell_scale_test.c`: the weakest enabled-control edge is 3.82:1 against the control's fill (>= 3:1, WCAG 1.4.11) in standard and 21:1 in high contrast. `scale_check.py` also looks for the edge colour in the real QEMU framebuffer. Cards, the dock panel and client bodies keep the lighter decorative `border`; disabled controls keep `border_dis` on purpose (they are not interactive and also say "not available yet").

**Hold-to-confirm demo (Cycle 22).** Index entry `Hold` opens a compositor-owned demo screen (SID 270 button, four segments, live status line) driven by the pure `hold.c` machine from REAL input events: a non-repeat Enter/Space key-down on the focused button or a pointer press inside it starts a hold; it confirms only in `shell_tick` after 1500 ms (1499 ms does not), exactly once per hold; release, the pointer leaving (re-entering does not resume), any other key, Tab/focus change, Escape, a modal or leaving the screen cancel it; key repeats are ignored; a release processed before the poll wins. It is a DEMONSTRATION: it performs no action, `env.hold_input` stays 0 and every S3 operation (delete, send, install, share) stays unavailable. No EL0 client can start, feed or confirm it. Evidence: `tests/kernel-host/shell_hold_test.c` (45 checks, mutation-checked: removing the key-release or modal cancel makes 6 fail) and `tests/qemu/hold-tests.sh` (real held key and button for real time, segment pixels, one logged confirmation). Real-hardware timing and a tamper-proof confirmation path are NOT claimed.
