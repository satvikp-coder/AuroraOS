# Program loader and trusted launch (issue #6, milestone D)

Status: implemented and tested in QEMU only (never on hardware). A program is read from AuroraFS on a virtual disk, validated, mapped into a
fresh isolated address space and run at EL0 under an identity chosen by the kernel. The machine code of the programs under test is **not in the
kernel binary**: the test script proves it by searching the kernel for the programs' marker strings (absent) and the disk image (present).

## What is supported

A deliberately narrow subset of ELF64 for AArch64 (`kernel/include/elf.h`, `kernel/proc/elf.c`):

| Required | Why |
|---|---|
| ELF64, little-endian, version 1, OSABI/ABI 0, zero padding, `ET_EXEC`, `EM_AARCH64`, `e_flags` 0 | static, non-PIE images only; no relocation exists in this loader |
| 1..8 program headers, 1..4 `PT_LOAD`, table inside the file and 8-byte aligned | bounded work |
| each `PT_LOAD` is exactly R, R+X or R+W (never W+X, never without R) | W^X |
| `p_align` = 4096 and vaddr ≡ file offset (mod 4096); `memsz` >= `filesz`; `memsz` > 0 | mapping is per 4 KiB page; `memsz - filesz` is zero-filled (bss) |
| segment wholly inside the load window `[USER_VA_BASE + 64 KiB, USER_VA_BASE + 1 MiB)` | the fixed low pages (code, data, **guard page**, **stack**) can never be overlapped |
| no two segments share a page; at most 12 pages in total | a page has one permission; bounded memory (the address space allows 24 pages including tables) |
| entry 4-byte aligned and inside the **file-backed** bytes of an executable segment | the first instruction is real, validated, executable code |
| file <= 24 KiB (the AuroraFS maximum) | |

Refused with a specific error: `PT_INTERP`, `PT_DYNAMIC`, `PT_TLS`, `PT_SHLIB`, unknown segment types, `ET_DYN`/PIE (needs relocation), an
executable `PT_GNU_STACK`, an empty (zero-size) `PT_LOAD`, truncated or overlapping or wrapping anything. Section headers are never read. There is no
dynamic linker, no shared library, no relocation, no Linux ABI, no `fork`/`exec`.

## What the loader does (`proc_create_elf`)

1. **Validate** the untrusted bytes again (every call; the description is never trusted separately from the bytes). No memory is touched yet.
2. Take a process slot in state `NEW`. The scheduler only runs `READY` processes, so nothing half-built can run.
3. Build the address space **page by page**: a kernel page buffer is zeroed, the file-backed bytes that fall in that page are copied in, and the page is
   mapped RX / RW (never executable) / RO from the validated flags. bss and the tail of the last file page are therefore zero. Code pages get a full
   data-cache clean and instruction-cache invalidate (`aspace_map`).
4. Map the stack (2 pages, RW, below an unmapped guard page) exactly as for built-in processes.
5. Only now set registers (`ELR` = entry, `SP_EL0` = stack top, `x0` = a number chosen by the kernel), the identity, and `READY`.

Any failure destroys the address space (which frees every page it owns) and releases the slot. **Tested** (kernel, `kernel/loadtest.c`): 20 hostile
images through the real path with the exact error codes; memory exhaustion at every depth (with exactly 0..10 free pages the load fails until 7 pages
exist - 3 tables, 2 image pages, 2 stack pages - and every failure and every destroy returns all pages); all 8 slots filled and the 9th refused; 300
mixed failing / memory-starved / successful loads leave page count and process table unchanged; no process ever remains in state `NEW`.

A loaded process has **no** fixed code page or data page (`code_pa` and `data_pa` are 0): only its image, its stack and the guard page. Addresses of the
fixed low pages fault.

## Trusted launch (who, which, as whom)

There is **no system call that starts a program**, names an executable or chooses an identity. Only kernel code can call `launch_app(name)`
(`kernel/proc/launch.c`, `kernel/include/launch.h`).

* **Who may request a launch:** kernel code only. (A future "start this application" request from EL0 would be a separate, authorised kernel service;
  it does not exist.)
* **Which executable:** the kernel's *registry* maps an application name to `(executable file name, identity, one number for x0)`. The executable is
  found with `fs_lookup_owner(PRINCIPAL_INSTALLER, file)`: the file must be owned by exactly the **installer** principal (`0x7F000001`). A same-named
  file owned by an application is never considered, so an application cannot supply or shadow an executable (tested: `shadow.elf`, owner 105, is
  ignored). Only kernel code can create or modify installer-owned files - no system call ever passes that principal and `launch_register` refuses it
  as an application identity - so an application cannot replace an installed executable either.
* **How it is trusted:** by *installer ownership* plus the integrity check, **not** by the checksum alone. The AuroraFS CRC only says the bytes are what
  was stored; it says nothing about whether the program is authorised or safe (a valid-checksum text file is refused by the ELF validator: tested).
  The file must also not be writable by others (`FS_PERM_OTHER_W` clear; tested). Whatever passes all of that still runs **unprivileged at EL0** in its
  own address space.
* **Which identity:** the principal in the **registry entry**. It is never derived from the file's owner, the caller or the file's contents. `0`, the
  none-value and the installer principal cannot be registered.
* **Which files the process can reach:** exactly those the AuroraFS permission rules give *that principal* (see storage.md): its own files, and files
  of others only where they granted other-read / other-write. Hidden files do not even reveal their existence (existence policy).
* **Why running another application's readable executable grants nothing:** the code and the identity are independent. The tested case: `alpha.elf`
  (readable by others, installed for identity 101) started under a second registry entry as application `clone` with identity 104 gets identity 104's
  empty namespace: it creates its own `alpha.dat`, cannot see or change identity 101's file, and 101's file is unchanged.
* **PID reuse:** the identity belongs to the registry entry, not the pid. `beta` started in the pid slot `alpha` just vacated is identity 102 and
  cannot see 101's data (tested).

## Provisioning in the tests

`tests/tools/mkafs.c` links the **real** `kernel/fs/afs.c` over a plain file (a block-layer shim in `tests/tools/shim/`) and writes the host-compiled
programs (`tests/programs/`) into a fresh disposable image as installer-owned files. The kernel then **mounts without formatting**. This is a
test-setup convenience and the only "installer" that exists; a real installation service is future work. Quotas apply to the installer principal like
any other (8 files, 96 blocks), which is why one `fault.elf` serves five fault kinds through the registry's numeric argument.

## Evidence (`tests/qemu/loader-tests.sh`, 72 checks, also in CI)

* three QEMU processes on the **same** disk image: the programs' counter files go 0 -> 1, 3 -> 4, 6 -> 7 (alpha) and 0 -> 1, 2 -> 3, 4 -> 5 (beta) -
  files written *by loaded EL0 programs* survive an emulator restart (issue #6 acceptance criterion "a program writes a file and it survives reboot");
* `alpha.elf` and `beta.elf` run **concurrently** at EL0 under identities 101 / 102, keep a `secret` variable at the **same virtual address**, write
  it, yield to each other and re-check it (isolation), verify initialised data / zeroed bss / readable rodata, cannot see each other's file, and are
  refused an unknown system call, an unknown file operation and a "format"-like operation;
* five deliberately faulting programs (write own code, jump to unmapped memory, privileged register read, kernel address read, write own rodata) are
  each `FAULTED` and the kernel carries on and launches a normal program afterwards;
* refused launches: unregistered name, missing executable, application-owned look-alike, other-writable executable, damaged data block
  (`FS_ECORRUPT`), valid file that is not an executable - none leaves a process or a page behind;
* the kernel binary contains no program marker string; the disk image does; an empty disk is untouched and only the in-RAM checks run.

Host fuzzing of the validator (`tests/kernel-host/elf_test.c`, ASan + UBSan): 61 negative cases, 300,000 mutation-fuzz iterations.

## Not done / limits

* QEMU only; the single CPU runs one process at a time; no hardware.
* No relocation, shared objects, dynamic linking, arguments beyond one kernel-chosen number, environment, `argv`, or exit-status delivery to a parent.
* Executables are authorised by installer ownership. There is no signature, no manifest, no rollback protection, no installer service or update path.
* AuroraFS has no digest stronger than CRC-32: it detects accidental corruption, not tampering by someone who can write the disk. The trust anchor
  is that only kernel code writes installer-owned files; physical access to the disk image defeats that (as it does for every file here).
* An image must give every segment content (the validator refuses empty `PT_LOAD`s; the test linker script guarantees a non-empty data segment).
* The registry is a small static kernel table (16 entries) filled by kernel code; production has no registered applications yet.
