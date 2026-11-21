#!/usr/bin/env python3
"""scripts/ci-local.py - run the `run:` steps of one job of .github/workflows/ci.yml on THIS machine, one after another, and print a pass/fail line per step.

Use it when GitHub Actions is unavailable (for example the monthly minutes are used up) to produce the same evidence locally. It is NOT a replacement for the
required checks: branch protection still wants the six GitHub jobs. Steps that install packages (apt, sudo, npm ci, pip install) are skipped because the
toolchain must already be present; they are listed as SKIPPED so nothing is hidden.

Usage: python3 scripts/ci-local.py [job-id]            default job: kernel-qemu-boot
       python3 scripts/ci-local.py kernel-qemu-boot --only storage,notes     run only steps whose name contains one of the words
Run it from the repository root (on the Linux/WSL side, where qemu-system-aarch64 and the aarch64 toolchain are installed).
"""
import subprocess, sys, time
try:
    import yaml
except ImportError:
    yaml = None


def parse_steps(path, job_id):
    """Minimal reader for this repository's workflow layout (no PyYAML needed): `jobs:` / `  <id>:` / `    steps:` / `      - name: ...` / `        run: |` blocks."""
    NL = chr(10); CR = chr(13)
    lines = open(path).read().replace(CR + NL, NL).split(NL)
    jobs, cur, steps, step, in_steps = [], None, [], None, False
    i = 0
    out = {}
    def flush():
        nonlocal step
        if step is not None and cur is not None: out.setdefault(cur, []).append(step)
        step = None
    while i < len(lines):
        l = lines[i]
        if l.startswith("  ") and not l.startswith("   ") and l.rstrip().endswith(":") and not l.strip().startswith("#"):
            flush(); cur = l.strip()[:-1]; out.setdefault(cur, []); in_steps = False
        elif l.startswith("    steps:"):
            in_steps = True
        elif in_steps and l.startswith("      - "):
            flush(); step = {}
            rest = l[8:]
            if rest.startswith("name:"): step["name"] = rest[5:].strip()
            elif rest.startswith("uses:"): step["uses"] = rest[5:].strip()
        elif in_steps and step is not None and l.startswith("        name:"): step["name"] = l.split(":", 1)[1].strip()
        elif in_steps and step is not None and l.startswith("        run:"):
            v = l.split(":", 1)[1].strip()
            if v in ("|", "|-", ">", ""):
                body = []
                i += 1
                while i < len(lines) and (lines[i].startswith("          ") or lines[i].strip() == ""):
                    body.append(lines[i][10:] if lines[i].startswith("          ") else ""); i += 1
                step["run"] = NL.join(body); continue
            step["run"] = v
        i += 1
    flush()
    return out.get(job_id)

SKIP_WORDS = ("apt-get", "apt ", "sudo ", "npm ci", "npm install", "pip install", "playwright install")

def main():
    args = sys.argv[1:]
    only = []
    if "--only" in args:
        i = args.index("--only"); only = [w for w in args[i + 1].split(",") if w]; del args[i:i + 2]
    job_id = args[0] if args else "kernel-qemu-boot"
    if yaml:
        wf = yaml.safe_load(open(".github/workflows/ci.yml"))
        if job_id not in wf["jobs"]:
            sys.exit("unknown job %r; jobs: %s" % (job_id, ", ".join(wf["jobs"])))
        steps = wf["jobs"][job_id]["steps"]
    else:
        steps = parse_steps(".github/workflows/ci.yml", job_id)
        if not steps: sys.exit("unknown job %r" % job_id)
    results = []
    for step in steps:
        name = step.get("name") or step.get("uses") or "step"
        run = step.get("run")
        if not run:
            continue
        if only and not any(w.lower() in name.lower() for w in only):
            continue
        if any(w in run for w in SKIP_WORDS):
            print("SKIPPED (installs software): %s" % name); results.append((name, "SKIPPED", 0)); continue
        print("=== %s" % name, flush=True)
        t0 = time.time()
        rc = subprocess.call(["bash", "-e", "-c", run])
        results.append((name, "PASS" if rc == 0 else "FAIL", time.time() - t0))
        print("--- %s: %s (%.0f s)" % (name, results[-1][1], results[-1][2]), flush=True)
    print("\n==== summary for %s ====" % job_id)
    for n, r, s in results: print("%-8s %5.0fs  %s" % (r, s, n))
    bad = [n for n, r, s in results if r == "FAIL"]
    print("%d step(s) run, %d failed, %d skipped" % (sum(1 for _, r, _ in results if r != "SKIPPED"), len(bad), sum(1 for _, r, _ in results if r == "SKIPPED")))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
