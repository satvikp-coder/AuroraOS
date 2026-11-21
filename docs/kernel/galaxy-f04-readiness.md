# Samsung Galaxy F04: documentary readiness assessment (research only)

Status: **NOT READY.** This is desk research. No phone was connected, no ADB/USB/Odin/fastboot command was run, nothing was flashed, unlocked or measured.
Every statement is marked with where it comes from. Anything not sourced is marked **UNVERIFIED** and must be checked before it is relied on.

## 1. What the sources say

| Item | Value | Source / confidence |
|---|---|---|
| Model | Samsung Galaxy F04, SM-E045F (and SM-E045F/DS) | [GSMArena](https://www.gsmarena.com/samsung_galaxy_f04-12054.php), [Notebookcheck](https://www.notebookcheck.net/Samsung-Galaxy-F04.694901.0.html) (consistent) |
| SoC | MediaTek MT6765 Helio P35. The process node is quoted as 12 nm by two sources and 14 nm by one: unresolved, irrelevant to OS work | GSMArena, Notebookcheck, Kimovil |
| CPU | 8 x Cortex-A53 (4 at 2.35 GHz, 4 at 1.8 GHz), AArch64-capable | GSMArena |
| GPU | PowerVR GE8320 | GSMArena |
| Memory / storage | 4 GB RAM, 64 GB internal storage | GSMArena |
| Display | 6.5 in, 720 x 1600 | GSMArena |
| Battery | 5000 mAh, 15 W | GSMArena |
| Shipped software | Android 12, One UI | GSMArena |
| Closest mainline-Linux status | postmarketOS lists the related Galaxy A04 (samsung-a04) as **not booting**; a community MT6765 mainline effort exists with only some components working | [postmarketOS wiki: Samsung Galaxy A04](https://wiki.postmarketos.org/wiki/Samsung_Galaxy_A04_(samsung-a04)), [MT6765 page](https://wiki.postmarketos.org/wiki/MediaTek_Helio_G35_(MT6765G)) |

The A04 family (A04, A04e, F04) shares the MT6765 platform with other Samsung phones (for example the A10s). The F04's exact board identity is **UNVERIFIED** until read from the device itself, which this assessment does not do.

## 2. What is NOT known (each one blocks hardware work)

1. **Bootloader policy.** Whether this model and carrier/region variant allows bootloader unlocking, and what unlocking does to Knox/warranty state. UNVERIFIED. (General knowledge, UNVERIFIED for this model: Samsung gates unlocking behind an "OEM unlocking" switch and a server-side wait, and unlocking is not reversible for the warranty/Knox state.)
2. **Boot chain.** The MediaTek preloader / secure-boot / verified-boot arrangement, which stage loads an arbitrary AArch64 image, and whether a signature check can be satisfied. UNVERIFIED. No evidence found that a custom image can be booted.
3. **Recovery path.** What can restore a device that stops booting (download mode with a matching signed firmware package, or a lower-level MediaTek mode). UNVERIFIED. Without a tested recovery path the device must be treated as bricked by any failed experiment.
4. **Kernel and device-tree sources.** Whether Samsung published the matching open-source kernel for SM-E045F. The search found community kernels for sibling devices but did **not** confirm a published source for the F04. UNVERIFIED.
5. **Peripheral documentation.** Display panel and controller, touch controller, eMMC/MSDC, power-management IC, charger, USB, modem, Wi-Fi/Bluetooth firmware blobs. No public datasheets were found for the MT6765 SoC itself; mainline coverage is partial.

## 3. What AuroraOS would need (gap analysis, from this repository, not from a device)

AuroraOS runs today only on QEMU `virt` (Cortex-A53 model, GICv2, virtio devices, ramfb). It has no code for any of the following, so the gap is large:

| Need | AuroraOS today |
|---|---|
| MT6765 interrupt controller, timers, UART early console | only GICv2/virt generic timer; MT6765 uses MediaTek-specific blocks (UNVERIFIED details) |
| eMMC (MSDC) block driver | virtio-blk only |
| Display bring-up (DSI panel, framebuffer handoff) | ramfb only |
| Touch input | virtio-tablet/keyboard only |
| Power, clocks, regulators, thermal | none |
| USB (even for debugging) | none |
| Wi-Fi/Bluetooth/modem | none; vendor firmware is proprietary |
| Booting from the vendor boot chain | a flat QEMU kernel image; no board image, no device tree for this SoC |

QEMU results never transfer: a working virtio driver says nothing about MSDC, DSI or the PMIC.

## 4. Readiness gates and verdict

| Gate | State |
|---|---|
| G0 Documentation: SoC, boot chain, recovery path and kernel source for SM-E045F are documented from authoritative sources | **NOT MET** (SoC identified from retail specs only) |
| G1 Non-destructive identification (read-only facts from the device: exact board, bootloader state, firmware version) | **NOT MET** and requires the owner's separate authorization before the phone is connected to anything |
| G2 Boot research (any attempt to run a non-Samsung image) | **NOT MET**: needs a verified recovery path and a second, expendable device |
| G3 Hardware experiment (flashing, unlocking, partition changes) | **NOT MET**: needs separate explicit owner approval after G0 to G2 |

**Verdict: NOT READY.** AuroraOS cannot be said to be able to boot on this phone. The nearest honest next steps are documentary: confirm whether Samsung published the SM-E045F kernel source, read the MT6765 mainline effort's driver list, and write the non-destructive identification plan (what to read and how, with the phone's data and warranty untouched) for the owner to approve or reject. Nothing in this repository authorizes touching the phone.

## 5. Safety rules that stay in force

No connecting the phone, no ADB, no USB debugging, no Odin, no fastboot, no flashing, no rooting, no bootloader unlocking, no partition changes. Any hardware step needs a new, explicit owner approval after a recovery plan exists. Do not use a daily-use phone for experiments.
