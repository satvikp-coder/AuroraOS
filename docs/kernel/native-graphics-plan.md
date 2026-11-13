# AuroraOS native graphics: research and plan (issue #7, stage G1)

**Status: research only. No kernel code changed and nothing was drawn.** The kernel has a UART console and nothing else for output. Everything below targets
QEMU `virt` only, run under `-cpu cortex-a53`; **nothing here was run on real hardware** and no claim is made about real hardware, GPUs or phones.

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

## 6. G2 design (not implemented)

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

## 9. Open decisions for the owner

1. Resolution for G2 (proposed 1024x768) and whether to support more than one.
2. The font source and licence for G4.
3. Whether the compositor lives in the kernel at first or goes straight to an EL0 server (the process and IPC model exists).
4. Whether to extract a shared virtio transport soon (it helps `virtio-gpu` and `virtio-input`) or only when G5 needs it.
5. Keep ramfb only as a bring-up path, or keep it supported long-term.

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
