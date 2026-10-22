# miniosv

A radically slimmed-down hard fork of the [OSv](https://github.com/cloudius-systems/osv) unikernel. Mostly done by [Claude Opus 4.8](https://www.anthropic.com/claude/opus).

## Motivation

OSv is a full-featured, Linux-compatible unikernel: ELF loader, syscall ABI, ZFS/ROFS/RAMFS,
a networking stack, loadable modules, and a GNU/musl toolchain. miniosv strips all of that away
to expose the smallest core that still boots and runs a real program — a single statically-linked
binary where the kernel and the application are compiled together, booted diskless, with no
syscall layer, no filesystem, and no networking. The goal is understanding and experimentation,
not Linux compatibility.

## What's left

- **Single binary** — the app is compiled into the kernel and entered via `osv_app_main()`; no ELF loader.
- **UEFI boot** — static `EXEC`, no `.dynamic`. Both arches boot only via UEFI: the kernel ELF is wrapped as an EFI application and loaded from a GPT/ESP disk image by the firmware (OVMF/AAVMF), the same path AWS, GCP and Azure take. No `-kernel`/PVH/multiboot.
- **No syscall ABI** — the app calls libc/kernel functions directly (zero syscall instructions).
- **Pure-LLVM toolchain** — clang + `ld.lld` + llvm-libc + libc++ + libunwind + compiler-rt, plus a small OSv-owned libc core. No GNU toolchain (no binutils, no libgcc); aarch64 cross-compiles by target triple with no GNU cross sysroot.
- Builds and boots on both **x86-64** and **aarch64**, under QEMU and on AWS, GCP and Azure.

## Build & run

```bash
make                               # build the x86-64 boot image -> build/release.x64/loader.img
./scripts/run.py                   # boot it under QEMU + UEFI (uses KVM when available)

make arch=aarch64                  # build the aarch64 image -> build/release.aarch64/loader.img
./scripts/run.py --arch aarch64    # boot it under QEMU + UEFI (TCG on an x86 host)
```

`make` produces a bootable GPT/ESP disk image (`loader.img`); `run.py` boots it as an NVMe disk
and puts the guest serial console on your terminal (Ctrl-A C for the QEMU monitor, Ctrl-A X to
quit). Requirements: QEMU, the UEFI firmware (`ovmf` for x86-64, `qemu-efi-aarch64` for aarch64),
and `mtools` + `gdisk` to build the disk image. Override the firmware with the `OVMF_CODE`/
`OVMF_VARS` or `AAVMF_CODE`/`AAVMF_VARS` env vars. `make smoke-test` boots the image headless and
checks it reaches the kernel.

The default app (`app/app.cc`) is a hello-world placeholder: it prints a greeting and then spins,
so the boot output stays on the console. Build the test suite instead with `make app=tests` — it
is a conformance gate that prints a `PASS`/`FAIL` line per check.

## Energy, DVFS and temperature (RAPL / CPPC)

`include/osv/perf.hh` extends the hardware-counter API beyond the PMU with energy readings,
per-cpu performance requests and temperature sensors. Everything is x86-64 only; on aarch64 the
backend is a stub that reports "not available" (energy and temperature read NaN, requests are
refused), so code using it builds and runs unchanged on both arches.

**RAPL energy.** `perf::EnergyEvent` measures the joules used in one `perf::EnergyDomain`
between `start()` and `stop()`. It needs no PMU counter, since the RAPL counters always run.
Vendors expose different domains, so each domain has one fixed meaning and reads NaN where it
doesn't exist:

| Domain  | Meaning                                       | AMD (Zen 2+) | Intel |
|---------|-----------------------------------------------|:------------:|:-----:|
| `PKG`   | whole package                                 | ✓            | ✓     |
| `CORE`  | physical core of the calling cpu              | ✓            |       |
| `CORES` | all cores of the package together (PP0)       |              | ✓     |
| `DRAM`  | memory attached to the package                |              | where present |

The counters are 32 bits wide and wrap after tens of kJ: `stop()` catches one wrap, and calling
`poll()` more often than that catches the rest. Intel server parts that use the fixed 15.3 µJ
DRAM unit are handled. `perf::PerfEvent` records all four domains alongside its PMU events, so
reports always have the same `energy-*-J` columns, plus a derived `power-pkg-W`.
`getEnergy(domain)` and `getPower(domain)` return the values directly.

**CPPC / HWP performance requests.** AMD CPPC and Intel HWP both take a min/max/desired
performance level and an energy-performance preference (EPP) per cpu, on the same abstract 0–255
scale. Both are mapped onto `perf::CppcCaps` and `perf::CppcRequest`:

```cpp
if (auto caps = perf::cppc_caps()) {          // nullopt if unavailable or nonsensical
    perf::CppcScope fixed(caps->nominal);     // min = max = desired, EPP kept; pins the thread
    if (fixed.ok()) { /* run at nominal */ }
}                                             // previous request restored here
```

`perf::cppc_request()` and `perf::cppc_set_request()` give raw access. `CppcScope` disables
migration while it is alive, because a request belongs to the cpu it was written on.

**Temperature.** `perf::temperature(TempDomain::PKG | CORE)` returns degrees Celsius (AMD Tctl,
Intel package or per-core DTS). On AMD, `temperature_ccd_mask()` and `temperature_ccd(n)` read
the per-CCD sensors.

**Running under KVM.** KVM virtualizes none of this. The guest only sees it when the host passes
the MSRs through, and every access uses `rdmsr_safe`, so a missing MSR reads as "not available"
instead of faulting:

- RAPL and CPPC/HWP need the host module `dvfs_passthrough.ko`, loaded before the VM starts.
  A power unit of 0, which is what KVM returns with `ignore_msrs`, is treated as missing.
- AMD temperature needs `smn_passthrough.ko`; Intel needs thermal-MSR passthrough.
- Readings are those of the physical hardware: `PKG` includes the host and other VMs, and
  `CORE`/CPPC refer to the physical cpu the vCPU runs on. Pin vCPUs 1:1 to host cpus (e.g. via
  QEMU's thread affinity or `taskset`) for per-core results to make sense. A CPPC request remains in effect on the host cpu
  after the VM exits, until the host restores its own (`dvfs_passthrough` `enable=0` or unload).

**Tests.** `make app=tests` includes `os-perf`, which checks that RAPL readings are plausible
where a domain exists and NaN where it doesn't, that per-core energy follows a single busy cpu,
that the same work runs at three fixed CPPC levels with the clock following and the request
restored afterwards, and that temperatures are plausible. Finally it runs all cpus hot, then
cold, and checks that clock, power and temperature follow.

## License

3-clause BSD (see [`LICENSE`](LICENSE)), inherited from OSv. Includes musl-derived code under the
MIT license (see [`LICENSE-musl`](LICENSE-musl)).
