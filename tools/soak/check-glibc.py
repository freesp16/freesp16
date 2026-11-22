#!/usr/bin/env python3
"""Release gate for the device binary (the release hardening checklist as gates): reads the ELF with the
host's GNU readelf (it reads any architecture; no toolchain container needed) and checks what the unit can run.

  tools/soak/check-glibc.py out/armel/<name> [more ELF files...]

Gates (exit 1 when one fails):
  - ARM, 32-bit EABI, soft-float ABI (armel: the firmware's userland), an ELF executable, interpreter /lib/ld-linux.so.3
  - every GLIBC_x.y symbol version <= 2.13 (the unit's C library)
  - no GLIBCXX_* / CXXABI_* / GCC_* (>= 4.6) versions: libstdc++ and libgcc are linked statically (toolchain/cross.cmake;
    the firmware's own libstdc++ is GCC 4.6's)
  - DT_NEEDED: no EGL / GLES / GAL (Vivante) library — the GL backends dlopen() them; every other needed
    library is listed (the firmware provides libc, libm, libdl, libpthread, librt, libasound, libX11, libfontconfig)
  - no RPATH / RUNPATH pointing into a build machine (/home, /work, /tmp)
Reported, not gated (our choice, performance first — the release flags are -O3 without -fstack-protector-strong /
_FORTIFY_SOURCE so the audio path pays nothing; the hardware run decides whether they can be afforded): stack
protector (__stack_chk_fail), FORTIFY (__*_chk imports), GNU_RELRO, BIND_NOW, an executable stack (GNU_STACK RWE is
a failure).
Standard library + readelf only.
"""
import re
import subprocess
import sys

GLIBC_MAX = (2, 13)
FORBIDDEN_NEEDED = re.compile(r"(EGL|GLES|GAL|libGL\b|vivante)", re.I)
BUILD_PATHS = ("/home/", "/work", "/tmp/", "/Users/")


def readelf(args, path):
    r = subprocess.run(["readelf", "-W", *args, path], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or f"readelf {' '.join(args)} failed")
    return r.stdout


def check(path):
    fails, notes = [], []
    try:
        header = readelf(["-h"], path)
        dyn = readelf(["-d"], path)
        progs = readelf(["-l"], path)
        vers = readelf(["-V"], path)
        syms = readelf(["--dyn-syms"], path)
    except (RuntimeError, FileNotFoundError) as e:
        return [f"cannot read {path}: {e}"], notes
    machine = re.search(r"Machine:\s+(.*)", header)
    flags = re.search(r"Flags:\s+(.*)", header)
    cls = re.search(r"Class:\s+(\S+)", header)
    kind = re.search(r"Type:\s+(\S+)", header)
    if not machine or machine.group(1).strip() != "ARM":
        fails.append(f"machine {machine.group(1).strip() if machine else '?'} (ARM expected)")
    if not cls or cls.group(1) != "ELF32":
        fails.append(f"class {cls.group(1) if cls else '?'} (ELF32 expected)")
    fl = flags.group(1) if flags else ""
    if "soft-float" not in fl or "EABI" not in fl:
        fails.append(f"flags '{fl}' (EABI, soft-float ABI expected: armel)")
    notes.append(f"type {kind.group(1) if kind else '?'}, {fl}")
    interp = re.search(r"Requesting program interpreter: (\S+)\]", progs)
    if not interp or interp.group(1) != "/lib/ld-linux.so.3":
        fails.append(f"interpreter {interp.group(1) if interp else 'none'} (/lib/ld-linux.so.3 expected)")
    versions = sorted(set(re.findall(r"Name: ((?:GLIBC|GLIBCXX|CXXABI|GCC)_[0-9.]+)", vers)))
    glibc = [v for v in versions if v.startswith("GLIBC_")]
    too_new = [v for v in glibc if tuple(int(x) for x in v[6:].split(".") if x) > GLIBC_MAX]
    if too_new:
        fails.append("glibc symbol versions newer than 2.13: " + ", ".join(too_new))
    notes.append("glibc versions: " + (", ".join(glibc) or "none"))
    cxx = [v for v in versions if not v.startswith("GLIBC_")]
    cxx_bad = [v for v in cxx if not v.startswith("GCC_") or tuple(int(x) for x in v[4:].split(".") if x) >= (4, 6)]
    if cxx_bad:
        fails.append("dynamic libstdc++ / libgcc versions (link them statically): " + ", ".join(cxx_bad))
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.*?)\]", dyn)
    bad = [n for n in needed if FORBIDDEN_NEEDED.search(n)]
    if bad:
        fails.append("GL libraries in DT_NEEDED (must be dlopen'ed): " + ", ".join(bad))
    notes.append("needs: " + ", ".join(needed))
    for tag in ("RPATH", "RUNPATH"):
        m = re.search(rf"\({tag}\)\s+Library (?:rpath|runpath): \[(.*?)\]", dyn)
        if m and any(p in m.group(1) for p in BUILD_PATHS):
            fails.append(f"{tag} points into a build machine: {m.group(1)}")
    stack = re.search(r"GNU_STACK\s+(?:\S+\s+){5}(\S+)", progs)
    if stack and "E" in stack.group(1):
        fails.append("executable stack (GNU_STACK RWE)")
    relro = "GNU_RELRO" in progs
    bindnow = bool(re.search(r"\(FLAGS\).*BIND_NOW|\(FLAGS_1\).*NOW", dyn))
    ssp = "__stack_chk_fail" in syms
    fortify = len(set(re.findall(r"__\w+_chk@", syms)))
    notes.append(f"hardening (reported): stack protector {'yes' if ssp else 'no'}, FORTIFY imports {fortify}, "
                 f"RELRO {'yes' if relro else 'no'}, BIND_NOW {'yes' if bindnow else 'no'}")
    return fails, notes


def main(argv):
    if len(argv) < 2:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    status = 0
    for path in argv[1:]:
        fails, notes = check(path)
        print(f"== {path}")
        for n in notes:
            print(f"   {n}")
        for f in fails:
            print(f"   FAIL {f}")
        print(f"   {'PASS' if not fails else 'FAIL'}")
        status |= 1 if fails else 0
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
