# Samsung Galaxy F04 port feasibility

**NO-GO for hardware bring-up. GO for desk research and QEMU development.** Desk review dated 27 November 2026. No phone was connected, inspected, unlocked, flashed, or modified.

## Confirmed platform and variants

Samsung identifies the India Galaxy F04 as SM-E045F. Samsung lists Jade Purple and Opal Green with 4 GB physical RAM and 64 GB internal storage. The 8 GB figure includes RAM Plus. Its product annex lists SM-E045FLVG and SM-E045FZGG, and Samsung India's support page identifies the full green SKU as SM-E045FZGGINS. These retail identifiers do not establish which board, panel, or touch-controller revisions shipped. Samsung's firmware history documents Android 14 build E045FXXSHEZH1 for SM-E045F. The installed firmware on any individual device is unknown. [Samsung launch announcement][launch] · [Samsung model annex][sku] · [Samsung India support][support] · [Samsung firmware history][firmware]

Samsung names the MediaTek Helio P35. MediaTek specifies eight 64-bit Cortex-A53 cores, PowerVR GE8320 graphics, a 12 nm process and eMMC 5.1 support. The processor architecture is compatible with AuroraOS's ARM64 code. The boot contract, memory map, and peripherals still need board-specific evidence. [Samsung launch announcement][launch] · [MediaTek Helio P35 specifications][p35]

## Boot-chain and recovery evidence

- **Bootloader policy and handoff remain unverified.** Android Verified Boot describes a chain of trust and rollback protection. Samsung documents hardware-backed boot security and Knox state on supported products. Neither source establishes the F04's unlocking eligibility, accepted image format, trust policy, exception level, entry address, or bootloader handoff. Do not infer an entry route from another MediaTek or Galaxy model. [Android Verified Boot][avb] · [Samsung Knox security architecture][knox]
- **Board sources remain unverified.** A lookup for SM-E045F at Samsung Open Source Release Center returned a browser challenge during this review. The archive and its relationship to a specific firmware build remain unconfirmed. Linux v6.18's MediaTek DT build list has no F04 target or MT6765 entry. A MediaTek-submitted patch series provides historical SoC context. It does not establish a supported F04 board boot. [Samsung source lookup][osrc] · [Linux v6.18 DT build list][dtb] · [MediaTek patch series][soc-patch]
- **Recovery remains blocked.** Samsung documents Smart Switch emergency recovery after a failed vendor update. Its guide does not establish recoverability after a failed custom boot or damaged boot stage on the F04. An approved recovery plan would need a matching signed stock package, verified provenance and hashes, rollback compatibility for the exact build and variant, and independently corroborated recoverability. [Samsung Smart Switch recovery guide][recovery]

## Driver and evidence matrix

Linux references describe candidate interfaces. A Linux binding or driver provides a starting point for research before matching the exact hardware and implementing an AuroraOS driver.

| Subsystem | Verified or candidate evidence | F04-specific prerequisite |
|---|---|---|
| CPU, interrupts, timer, UART | Helio P35 is AArch64. MediaTek proposed basic MT6765 SoC support in a Linux patch series. AuroraOS uses QEMU's interrupt controller, generic timer and UART in its native path. | Establish boot exception level, interrupt routing, timer access and frequency, UART controller and pinmux, reserved memory and watchdog behavior. [MediaTek P35 specifications][p35] · [MediaTek patch series][soc-patch] |
| Clock and pinmux | Linux has MT6765 clock and pinctrl implementation references. | Match reference clocks and board pin state to the corresponding stock device tree before configuring anything. [Linux clock driver][clock] · [Linux pinctrl descriptors][pins] |
| Storage | P35 supports eMMC 5.1. Linux has MediaTek MSDC bindings. AuroraOS uses virtio-blk. | Confirm the F04 controller compatible, eMMC part, clocks, supply rails, reset and DMA constraints. Initial storage design must be read-only. [MediaTek P35 specifications][p35] · [Linux MSDC binding][msdc] |
| Display | Samsung confirms a 16.55 cm HD+ display. AuroraOS uses QEMU ramfb. | Identify display engine, panel and revision, link configuration, clocks, power sequencing, backlight and any documented boot framebuffer handoff. [Samsung launch announcement][launch] |
| Touchscreen | No F04 controller was verified. AuroraOS uses virtio input. | Resolve controller revisions, bus, interrupts, resets and firmware requirements from board-specific evidence. |
| USB | Linux has a MediaTek USB controller binding. The F04 controller, connector wiring and role-switch topology remain unverified for a native port. | Identify USB block and PHY, port role handling, power behavior and supported diagnostics. [Linux MediaTek USB binding][usb] |
| Audio | No F04 codec, routing or microphone path was verified. AuroraOS uses virtio-snd. | Identify codec, buses, DMA and routing. Preserve hardware mute, mic authorization and the trusted recording indicator. |
| Battery, charging, power | Samsung specifies 5,000 mAh. The F04 PMIC, charger, fuel gauge and thermal sensors were not verified. | Confirm supply rails, battery and thermal monitoring, safe shutdown and watchdog behavior. Charging changes require separate validation. [Samsung launch announcement][launch] |
| Wi-Fi and Bluetooth | MediaTek lists Wi-Fi 5 and Bluetooth 5.0 at SoC level. Samsung's board implementation and firmware interface remain unverified. | Identify connectivity chip, transport, calibration, firmware provenance and regulatory constraints. [MediaTek P35 specifications][p35] |
| Cellular | MediaTek lists LTE capability at SoC level. The F04 modem firmware, host interface and board-specific implementation remain unverified. | Defer cellular behind storage, display and power. Require a separate study of firmware, calibration, regulatory requirements and telephony behavior. [MediaTek P35 specifications][p35] |

## Porting roadmap

1. **Documentary evidence first.** Locate Samsung's source for the exact SM-E045F build and the matching, legitimately obtained stock metadata. Record the release, file hashes and license terms. Inspect source offline for board identifiers, DT/overlay entries, peripheral variants, boot-image layout, memory reservations and boot handoff. Keep the phone disconnected. A missing source archive or unconfirmed board match leaves the corresponding fixture and driver blocked.
2. **Keep the first implementation QEMU-only.** After the public QEMU beta milestone, validate any reusable boot-parameter parsing and platform interfaces against QEMU and synthetic fixtures. Require deterministic rejection of unsupported board IDs, malformed mappings and overlapping memory reservations before introducing a physical-board target.
3. **Require a separate hardware decision.** Reassess only when the exact board is identified, a legitimate boot path is documented, recovery is independently corroborated, and the owner explicitly authorizes a separate experiment on a spare device. Initial goals would be early diagnostics, timer and interrupt operation, and controlled shutdown. Any phase requiring device access is blocked under the current assignment.
4. **Order peripheral work by dependency.** Establish clock, power and reset dependencies before read-only storage, then display and input. Follow with controlled writes and recovery tests, audio, and finally connectivity. Keep stock partitions, calibration and radio state outside the OS's write targets.

Check SPDX identifiers and distribution terms before incorporating any reference code. Review board firmware, modem and connectivity binaries separately. Availability of a vendor source archive would not establish permission to redistribute firmware or copy drivers into AuroraOS. Keep the port independent and record provenance for each implementation. [Linux kernel licensing rules][license]

[launch]: https://news.samsung.com/in/samsung-launches-galaxy-f04-with-up-to-8gb-ram-with-ram-plus-for-fast-performance
[sku]: https://images.samsung.com/is/content/samsung/assets/in/unpacked/ANNEXURE-A.pdf
[support]: https://www.samsung.com/in/support/model/SM-E045FZGGINS/
[firmware]: https://doc.samsungmobile.com/SM-E045F/025905230224/eng.html
[p35]: https://www.mediatek.com/products/smartphones/mediatek-helio-p35
[avb]: https://source.android.com/docs/security/features/verifiedboot
[knox]: https://docs.samsungknox.com/admin/fundamentals/whitepaper/samsung-knox-mobile-security/system-security/hw-backed-security/
[osrc]: https://opensource.samsung.com/uploadSearch?searchValue=SM-E045F
[dtb]: https://github.com/torvalds/linux/blob/v6.18/arch/arm64/boot/dts/mediatek/Makefile
[soc-patch]: https://lists.infradead.org/pipermail/linux-mediatek/2021-March/022343.html
[recovery]: https://update.kies.samsung.com/SmartSwitchPC_recovery_guide/firmware/emergencyrecovery/
[clock]: https://linux.googlesource.com/linux/kernel/git/torvalds/linux/%2B/077ab8985ee278c3d8618182d335b0f0cd919e16/drivers/clk/mediatek/clk-mt6765.c
[pins]: https://github.com/torvalds/linux/blob/v6.18/drivers/pinctrl/mediatek/pinctrl-mtk-mt6765.h
[msdc]: https://github.com/torvalds/linux/blob/v6.18/Documentation/devicetree/bindings/mmc/mtk-sd.yaml
[usb]: https://github.com/torvalds/linux/blob/v6.18/Documentation/devicetree/bindings/usb/mediatek%2Cmtu3.yaml
[license]: https://docs.kernel.org/process/license-rules.html
