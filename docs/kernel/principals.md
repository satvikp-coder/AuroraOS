# Principals and the reserved range

A principal is the 32-bit identity a process holds for file access and capabilities. Only kernel code assigns it (a launch registration or `proc_set_principal`); EL0 cannot choose or change it.

`kernel/include/principal.h` is the single policy: `principal_assignable(p)` is true only when `p` is not 0 (the kernel), not `PRINCIPAL_NONE` (0xFFFFFFFF) and not in the **reserved range 0x7F000000..0x7FFFFFFF**.

| Principal | Meaning | Held by a process? |
|---|---|---|
| 0x7F000001 | installer, owns installed executables | never |
| 0x7F000002 | settings, owns `settings.v1` | never |
| 0x7F000003 | the trusted shell (requester pid 0, not a process) | never |
| 0x7F000004 | Passport, will own the permission ledger files | never |
| 0x7F000005 | Trail, will own the audit ledger files | never |

Enforced at every assignment path (each has a boot self-test and the host test `principal_test.c`): `proc_create_elf`, `proc_set_principal`, `launch_register`, `ui_sys_grant`, `timersys_grant`. A future reserved number is protected automatically because the rule is the range, not a list. The reserved identities exist only as file owners and as in-kernel service identities; the shell's settings rights are granted to `PRINCIPAL_SHELL` by kernel code inside `settings.c`, which is intentionally outside this process-side policy.

Not in this change: the Passport/Trail ledger files themselves (next PRs).
