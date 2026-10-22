#pragma once

// x86 PMU back-end for osv/perf.hh.
// Targets AMD Zen 4/5 and Intel Skylake and newer.

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>

#include "apic.hh"
#include "exceptions.hh"
#include "osv/perf.hh"
#include "processor.hh"

namespace perf {

enum class CpuVendor { AMD, INTEL, UNKNOWN };

inline CpuVendor cpu_vendor() {
  processor::cpuid_result r = processor::cpuid(0);
  char v[13];
  std::memcpy(v + 0, &r.b, sizeof(r.b));
  std::memcpy(v + 4, &r.d, sizeof(r.d));
  std::memcpy(v + 8, &r.c, sizeof(r.c));
  v[12] = '\0';
  if (std::memcmp(v, "GenuineIntel", 12) == 0)
    return CpuVendor::INTEL;
  if (std::memcmp(v, "AuthenticAMD", 12) == 0)
    return CpuVendor::AMD;
  return CpuVendor::UNKNOWN;
}

inline bool is_intel() { return cpu_vendor() == CpuVendor::INTEL; }
inline bool is_amd() { return cpu_vendor() == CpuVendor::AMD; }

// Identifies the cpu design. On a hybrid part the P and E cores report
// different signatures and expose different counter counts, so anything cached
// per-PMU has to be keyed on this rather than probed once on the boot cpu.
inline uint32_t pmu_design_id() { return processor::cpuid(1).a; }

inline uint32_t pmu_num_counters() {
  if (is_intel()) {
    // Intel: CPUID leaf 0x0A EAX[15:8] = number of general-purpose PMCs.
    processor::cpuid_result r = processor::cpuid(0x0A);
    return (r.a >> 8) & 0xFF;
  }
  // AMD: prefer PerfMonV2 (CPUID Fn8000_0022h) which reports the counter
  // count directly. This is architectural from Zen 4 onwards.
  processor::cpuid_result ext_max = processor::cpuid(0x80000000);
  if (ext_max.a >= 0x80000022u) {
    processor::cpuid_result r = processor::cpuid(0x80000022);
    if (r.a & 0x1u)
      return r.b & 0xFu;
  }
  // Fall back: PerfCtrExtCore (CPUID Fn8000_0001h_ECX[23]) gates the extended
  // 6-counter MSR range; legacy AMDs expose 4.
  processor::cpuid_result r = processor::cpuid(0x80000001);
  return (r.c & (1u << 23)) ? 6 : 4;
}

// x86 has no MIDR register — shim for arch-neutral callers.
inline bool is_midr(uint32_t) { return false; }

// KVM-guest probing is only used by the ARM Neoverse V1 workaround.
inline bool is_kvm_guest() { return false; }

inline constexpr uint32_t intel_msr_perf_global_ctrl = 0x38Fu;

inline void enable_pmu() {
  // AMD PMU is enabled by default; nothing to do.
  if (!is_intel())
    return;
  uint32_t n = pmu_num_counters();
  if (n == 0)
    return;
  uint64_t mask = (n >= 32) ? 0xFFFF'FFFFull : ((1ull << n) - 1);
  processor::wrmsr(intel_msr_perf_global_ctrl, mask);
}

inline void pmc_stop(uint32_t evt_sel) { processor::wrmsr(evt_sel, 0); }

inline void pmc_write_counter(uint32_t ctr, uint64_t value) {
  processor::wrmsr(ctr, value);
}

inline bool pmc_start_with_conf(uint32_t /*ctr*/, uint32_t evt_sel,
                                uint64_t value) {
  processor::wrmsr(evt_sel, value);
  return true;
}

inline uint64_t pmc_read(uint32_t ctr) { return processor::rdmsr(ctr); }

inline constexpr uint64_t pmc_int_enable = 1ull << 20;

using PMCIntHandle = unsigned;

// Fixed in hardware; no x86 bit widens it. AMD core PMCs are 48 bits.
inline uint32_t pmc_overflow_width(uint32_t = 0) {
  if (is_intel()) {
    uint32_t width = (processor::cpuid(0x0A).a >> 16) & 0xFFu;
    if (width)
      return width;
  }
  return 48;
}

inline uint64_t pmc_counter_mask(uint32_t ctr = 0) {
  uint32_t width = pmc_overflow_width(ctr);
  return width >= 64 ? ~0ull : ((1ull << width) - 1);
}

inline uint64_t pmc_period_value(uint32_t ctr, uint64_t period) {
  return -period & pmc_counter_mask(ctr);
}
inline constexpr uint32_t amd_msr_perf_cntr_global_status_clr = 0xC0000302u;
inline constexpr uint32_t intel_msr_perf_global_ovf_ctrl = 0x390u;
inline constexpr uint32_t intel_msr_perf_capabilities = 0x345u;

struct PMCOverflowAck {
  uint32_t msr;
  uint64_t mask;
};

// x86 counters are at least 48 bits wide, so wrap counting is never enabled
// here and these exist only to satisfy the arch-neutral front-end.
inline uint64_t pmu_overflow_status() { return 0; }
inline uint64_t pmc_overflow_bit(uint32_t) { return 0; }
inline void pmc_ack_overflow_mask(uint64_t, PMCIntHandle) {}
inline void pmc_disable_overflow_int(uint64_t) {}

inline PMCOverflowAck pmc_overflow_ack_conf(uint32_t) {
  if (is_intel())
    return {intel_msr_perf_global_ovf_ctrl, (1ull << pmu_num_counters()) - 1};
  processor::cpuid_result ext_max = processor::cpuid(0x80000000);
  if (ext_max.a >= 0x80000022u && (processor::cpuid(0x80000022).a & 0x1u))
    return {amd_msr_perf_cntr_global_status_clr,
            (1ull << pmu_num_counters()) - 1};
  return {0, 0};
}

// Drop a pending overflow without touching the interrupt controller.
inline void pmc_clear_overflow(PMCOverflowAck ack) {
  if (ack.mask)
    processor::wrmsr(ack.msr, ack.mask);
}

// x86 routes the PMU interrupt through LVTPC, which carries a single vector,
// so a handler only ever runs for its own counter.
inline bool pmc_overflow_pending(PMCOverflowAck) { return true; }

// IA32_PMCx writes are 32-bit sign-extended; the IA32_A_PMCx aliases take the
// full counter width, which sampling periods beyond 2^31 need.
inline bool intel_full_width_write() {
  return (processor::cpuid(1).c & (1u << 15)) &&
         (processor::rdmsr(intel_msr_perf_capabilities) & (1ull << 13));
}

inline PMCIntHandle pmc_attach_overflow_handler(std::function<void()> handler) {
  unsigned vector = idt.register_handler(std::move(handler));
  processor::apic->write(processor::apicreg::LVTPC, vector);
  return vector;
}

inline void pmc_detach_overflow_handler(PMCIntHandle vector, uint64_t /*mask*/) {
  processor::apic->write(processor::apicreg::LVTPC, 1u << 16);
  idt.unregister_handler(vector);
}

inline void pmc_ack_overflow(PMCOverflowAck ack, PMCIntHandle vector) {
  if (ack.mask)
    processor::wrmsr(ack.msr, ack.mask);
  processor::apic->write(processor::apicreg::LVTPC, vector);
}

namespace PERF_COUNT_HW {
// PerfEvtSel encoding: bit22=EN, bit17=OS, bit16=USR, bits8-15=UMask,
// bits0-7=Event Select. Both vendors share this layout.
constexpr uint64_t encode(uint8_t event, uint8_t umask) {
  return 0x430000ull | (static_cast<uint64_t>(umask) << 8) | event;
}

using enum PMClass;

// Vendor-specific event catalogues. Codes and UMasks are taken from:
//   - AMD Family 19h (Zen 3/4) Processor Programming Reference, PMCx event
//     table.
//   - Intel SDM Vol 3B, chapters 19-20 (architectural + Skylake/Ice Lake
//     table). Events picked here work on Skylake and later.
// clang-format off
namespace AMD {
constexpr PMCEvent CPU_CYCLES        = {encode(0x76, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INSTRUCTIONS      = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent UOPS_RETIRED      = {encode(0xC1, 0x00), CORE, "uops-retired"};
constexpr PMCEvent BRANCH_PREDICTION = {encode(0xC2, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent BRANCH_MISS       = {encode(0xC3, 0x00), CORE, "branch-misses"};
constexpr PMCEvent BRANCH_TAKEN      = {encode(0xC4, 0x00), CORE, "branch-taken"};
constexpr PMCEvent STALL_FRONTEND    = {encode(0xA9, 0x00), CORE, "stall-frontend"};
// PMCx029 (LsDispatch): loads+stores+load-stores dispatched to the LS unit —
// i.e. all L1 data cache accesses.
constexpr PMCEvent L1D_ACCESSES      = {encode(0x29, 0x07), CORE, "l1d-accesses"};
// PMCx045 (LsL1DTlbMiss). UMask 0xFF = any L1 DTLB miss;
// UMask 0xF0 = L1+L2 DTLB miss, i.e. a full page walk was required
// (parallel to Intel's *_WALK_COMPLETED).
constexpr PMCEvent DTLB_L1_MISS      = {encode(0x45, 0xFF), CORE, "dtlb-l1-miss"};
constexpr PMCEvent DTLB_WALK         = {encode(0x45, 0xF0), CORE, "dtlb-walk"};
constexpr PMCEvent TLB_FLUSHES       = {encode(0x78, 0xFF), CORE, "tlb-flushes"};
// PMCx064 (L2CacheReqStat) UMask 0x09 = cacheable read blocks, i.e. L2 read
// misses. AMD has no true L3-miss core PMC, so this backs LL_CACHE_MISS too.
constexpr PMCEvent L2D_CACHE_MISS   = {encode(0x64, 0x09), CORE, "l2d-cache-misses"};
} // namespace AMD

namespace INTEL {
constexpr PMCEvent CPU_CYCLES        = {encode(0x3C, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INSTRUCTIONS      = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent UOPS_RETIRED      = {encode(0xC2, 0x02), CORE, "uops-retired"};
constexpr PMCEvent BRANCH_PREDICTION = {encode(0xC4, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent BRANCH_MISS       = {encode(0xC5, 0x00), CORE, "branch-misses"};
constexpr PMCEvent STALL_FRONTEND    = {encode(0x9C, 0x01), CORE, "stall-frontend"};
// MEM_LOAD_RETIRED.L1_MISS.
constexpr PMCEvent L1D_CACHE_MISS    = {encode(0xD1, 0x08), CORE, "l1d-cache-misses"};
// MEM_LOAD_RETIRED.L2_MISS.
constexpr PMCEvent L2D_CACHE_MISS    = {encode(0xD1, 0x10), CORE, "l2d-cache-misses"};
// MEM_INST_RETIRED.ALL_LOADS / ALL_STORES.
constexpr PMCEvent LOADS_RETIRED     = {encode(0xD0, 0x81), CORE, "loads-retired"};
constexpr PMCEvent STORES_RETIRED    = {encode(0xD0, 0x82), CORE, "stores-retired"};
// DTLB_LOAD_MISSES.WALK_COMPLETED / ITLB_MISSES.WALK_COMPLETED — count of
// fully-completed page walks (any page size).
constexpr PMCEvent DTLB_LOAD_WALK    = {encode(0x08, 0x0E), CORE, "dtlb-load-walk"};
constexpr PMCEvent ITLB_WALK         = {encode(0x85, 0x0E), CORE, "itlb-walk"};
// LONGEST_LAT_CACHE.REFERENCE / .MISS.
constexpr PMCEvent LL_CACHE         = {encode(0x2E, 0x4F), CORE, "ll-cache-accesses"};
constexpr PMCEvent LL_CACHE_MISS    = {encode(0x2E, 0x41), CORE, "ll-cache-misses"};
} // namespace INTEL
// clang-format on

// Vendor-neutral aliases picked at runtime. Events without a good match on
// both vendors are exposed only under AMD::/INTEL:: above.
inline const PMCEvent CPU_CYCLES        = is_intel() ? INTEL::CPU_CYCLES        : AMD::CPU_CYCLES;
inline const PMCEvent INSTRUCTIONS      = is_intel() ? INTEL::INSTRUCTIONS      : AMD::INSTRUCTIONS;
inline const PMCEvent UOPS_RETIRED      = is_intel() ? INTEL::UOPS_RETIRED      : AMD::UOPS_RETIRED;
inline const PMCEvent BRANCH_PREDICTION = is_intel() ? INTEL::BRANCH_PREDICTION : AMD::BRANCH_PREDICTION;
inline const PMCEvent BRANCH_MISS       = is_intel() ? INTEL::BRANCH_MISS       : AMD::BRANCH_MISS;
inline const PMCEvent STALL_FRONTEND    = is_intel() ? INTEL::STALL_FRONTEND    : AMD::STALL_FRONTEND;
inline const PMCEvent L2D_CACHE_MISS    = is_intel() ? INTEL::L2D_CACHE_MISS   : AMD::L2D_CACHE_MISS;
// AMD has no true L3-miss core PMC; the LLC alias falls back to L2 there.
inline const PMCEvent LL_CACHE_MISS     = is_intel() ? INTEL::LL_CACHE_MISS    : AMD::L2D_CACHE_MISS;
// AMD does not expose a direct L2/LL access counter through the core PMCs; on
// AMD this alias reports L1D accesses instead (matches pre-existing behaviour).
inline const PMCEvent LL_CACHE          = is_intel() ? INTEL::LL_CACHE         : AMD::L1D_ACCESSES;
} // namespace PERF_COUNT_HW

// ---------------- RAPL energy ----------------
//
// KVM doesn't offer RAPL to guests; it is there when the host passes the MSRs
// through (e.g. dvfs_passthrough.ko on AMD, loaded before the VM started). So
// every access is a safe one, and a power unit of 0 counts as missing: that is
// what a KVM with ignore_msrs returns, real hardware never does.
//
// AMD (Zen 2 and later) has a CPUID bit and a per-core counter. Intel has no
// CPUID bit, its PP0 counter covers all cores of the package together, and it
// has a DRAM counter; see EnergyDomain.
inline constexpr uint32_t amd_msr_rapl_power_unit = 0xC0010299u;
inline constexpr uint32_t amd_msr_core_energy_status = 0xC001029Au;
inline constexpr uint32_t amd_msr_pkg_energy_status = 0xC001029Bu;
inline constexpr uint32_t intel_msr_rapl_power_unit = 0x606u;
inline constexpr uint32_t intel_msr_pkg_energy_status = 0x611u;
inline constexpr uint32_t intel_msr_dram_energy_status = 0x619u;
inline constexpr uint32_t intel_msr_pp0_energy_status = 0x639u;

inline bool amd_has_ext_leaf(uint32_t leaf) {
  return processor::cpuid(0x80000000).a >= leaf;
}

// The domain's energy counter on this vendor, or 0 if it has none.
inline uint32_t rapl_energy_msr(EnergyDomain domain) {
  if (is_amd()) {
    switch (domain) {
    case EnergyDomain::PKG: return amd_msr_pkg_energy_status;
    case EnergyDomain::CORE: return amd_msr_core_energy_status;
    default: return 0;
    }
  }
  if (is_intel()) {
    switch (domain) {
    case EnergyDomain::PKG: return intel_msr_pkg_energy_status;
    case EnergyDomain::CORES: return intel_msr_pp0_energy_status;
    case EnergyDomain::DRAM: return intel_msr_dram_energy_status;
    default: return 0;
    }
  }
  return 0;
}

inline bool rapl_power_unit(uint64_t &unit) {
  if (is_amd()) {
    // CPUID Fn8000_0007h_EDX[14] (RAPL).
    if (!amd_has_ext_leaf(0x80000007) ||
        !(processor::cpuid(0x80000007).d & (1u << 14)))
      return false;
    return processor::rdmsr_safe(amd_msr_rapl_power_unit, unit) && unit;
  }
  return is_intel() && processor::rdmsr_safe(intel_msr_rapl_power_unit, unit) &&
         unit;
}

// Intel Xeons from Haswell-EP on count DRAM energy in a fixed unit of 15.3 uJ
// instead of the one in RAPL_POWER_UNIT (as in Linux' intel_rapl driver).
inline bool intel_dram_fixed_unit() {
  uint32_t a = processor::cpuid(1).a;
  if (((a >> 8) & 0xF) != 6)
    return false;
  uint32_t model = ((a >> 4) & 0xF) | ((a >> 12) & 0xF0);
  switch (model) {
  case 0x3F: // Haswell-X
  case 0x4F: // Broadwell-X
  case 0x56: // Broadwell-D
  case 0x55: // Skylake-X, Cascade Lake, Cooper Lake
  case 0x57: // Xeon Phi (Knights Landing)
  case 0x85: // Xeon Phi (Knights Mill)
  case 0x6A: // Ice Lake-X
  case 0x6C: // Ice Lake-D
  case 0x8F: // Sapphire Rapids
  case 0xCF: // Emerald Rapids
  case 0xAD: // Granite Rapids-X
  case 0xAE: // Granite Rapids-D
  case 0xAF: // Sierra Forest
    return true;
  default:
    return false;
  }
}

inline bool rapl_available(EnergyDomain domain) {
  uint64_t unit, raw;
  uint32_t msr = rapl_energy_msr(domain);
  return msr && rapl_power_unit(unit) && processor::rdmsr_safe(msr, raw);
}

// Joules per counter increment: RAPL_POWER_UNIT[12:8] (ESU, the same field on
// both vendors) is -log2 of it.
inline double rapl_energy_unit(EnergyDomain domain) {
  uint64_t unit;
  if (!rapl_power_unit(unit))
    return NAN;
  if (domain == EnergyDomain::DRAM && is_intel() && intel_dram_fixed_unit())
    return 15.3e-6;
  return 1.0 / static_cast<double>(1ull << ((unit >> 8) & 0x1F));
}

// The energy counters are 32 bits wide and wrap.
inline constexpr uint32_t rapl_counter_width = 32;

inline bool rapl_read(EnergyDomain domain, uint64_t &raw) {
  uint32_t msr = rapl_energy_msr(domain);
  if (!msr || !processor::rdmsr_safe(msr, raw))
    return false;
  raw &= 0xFFFF'FFFFull;
  return true;
}

// ---------------- CPPC / HWP performance requests ----------------
//
// AMD CPPC and Intel HWP are the same idea with different registers: both take
// a min/max/desired performance level and an energy-performance preference,
// per cpu, in the same abstract 0-255 scale. The field order differs (AMD has
// max first, Intel min), so both are translated to CppcRequest here. A CPUID
// bit doesn't guarantee each MSR (Zen 2 has CPPC but no CPPC_STATUS), so these
// are safe accesses as well.
inline constexpr uint32_t amd_msr_cppc_cap1 = 0xC00102B0u;
inline constexpr uint32_t amd_msr_cppc_enable = 0xC00102B1u;
inline constexpr uint32_t amd_msr_cppc_req = 0xC00102B3u;
inline constexpr uint32_t intel_msr_pm_enable = 0x770u;
inline constexpr uint32_t intel_msr_hwp_capabilities = 0x771u;
inline constexpr uint32_t intel_msr_hwp_request = 0x774u;

inline uint8_t msr_byte(uint64_t v, unsigned n) {
  return static_cast<uint8_t>(v >> (8 * n));
}

inline bool cppc_available() {
  uint64_t enable;
  if (is_amd()) {
    // CPUID Fn8000_0008h_EBX[27] (CPPC), and the host has enabled it.
    return amd_has_ext_leaf(0x80000008) &&
           (processor::cpuid(0x80000008).b & (1u << 27)) &&
           processor::rdmsr_safe(amd_msr_cppc_enable, enable) && (enable & 1);
  }
  // CPUID.06H:EAX[7] (HWP), and IA32_PM_ENABLE has it turned on.
  return is_intel() && (processor::cpuid(6).a & (1u << 7)) &&
         processor::rdmsr_safe(intel_msr_pm_enable, enable) && (enable & 1);
}

// AMD CPPC_CAP1: [31:24] highest, [23:16] nominal, [15:8] lowest nonlinear,
// [7:0] lowest. Intel HWP_CAPABILITIES: [7:0] highest, [15:8] guaranteed,
// [23:16] most efficient, [31:24] lowest.
inline bool cppc_read_caps(CppcCaps &caps) {
  uint64_t v;
  if (is_amd() && processor::rdmsr_safe(amd_msr_cppc_cap1, v)) {
    caps = {msr_byte(v, 3), msr_byte(v, 2), msr_byte(v, 1), msr_byte(v, 0)};
    return true;
  }
  if (is_intel() && processor::rdmsr_safe(intel_msr_hwp_capabilities, v)) {
    caps = {msr_byte(v, 0), msr_byte(v, 1), msr_byte(v, 2), msr_byte(v, 3)};
    return true;
  }
  return false;
}

// AMD CPPC_REQ: [31:24] EPP, [23:16] desired, [15:8] min, [7:0] max.
// Intel HWP_REQUEST: [31:24] EPP, [23:16] desired, [15:8] max, [7:0] min.
inline uint32_t cppc_request_msr() {
  return is_amd() ? amd_msr_cppc_req : is_intel() ? intel_msr_hwp_request : 0;
}

inline bool cppc_read_request(CppcRequest &req) {
  uint64_t v;
  uint32_t msr = cppc_request_msr();
  if (!msr || !processor::rdmsr_safe(msr, v))
    return false;
  if (is_amd())
    req = {msr_byte(v, 0), msr_byte(v, 1), msr_byte(v, 2), msr_byte(v, 3)};
  else
    req = {msr_byte(v, 1), msr_byte(v, 0), msr_byte(v, 2), msr_byte(v, 3)};
  return true;
}

// The bits above the four levels (Intel: activity window, package control)
// are written back as read.
inline bool cppc_write_request(const CppcRequest &req) {
  uint64_t v;
  uint32_t msr = cppc_request_msr();
  if (!msr || !processor::rdmsr_safe(msr, v))
    return false;
  uint8_t lo = is_amd() ? req.max : req.min;
  uint8_t hi = is_amd() ? req.min : req.max;
  v = (v & ~0xFFFF'FFFFull) | static_cast<uint64_t>(req.epp) << 24 |
      static_cast<uint64_t>(req.desired) << 16 |
      static_cast<uint64_t>(hi) << 8 | lo;
  return processor::wrmsr_safe(msr, v);
}

// ---------------- Temperature ----------------
//
// Intel has architectural thermal MSRs: a per-core and a package sensor, each
// read as degrees below TjMax, in whole degrees. AMD has none; its sensors are
// on the SMN (Tctl and one per CCD, in 0.125 degrees), which a guest can only
// read through the MSRs that smn_passthrough.ko emulates. Both are converted
// to absolute degrees Celsius here, as Linux' coretemp and k10temp do.
inline constexpr uint32_t intel_msr_therm_status = 0x19Cu;
inline constexpr uint32_t intel_msr_temperature_target = 0x1A2u;
inline constexpr uint32_t intel_msr_package_therm_status = 0x1B1u;
inline constexpr uint32_t smnw_msr_info = 0x400000C0u;
inline constexpr uint32_t smnw_msr_tctl = 0x400000C1u;
inline constexpr uint32_t smnw_msr_ccd0 = 0x400000C2u;
inline constexpr uint32_t smnw_magic = 0x574E4D53u; // "SMNW"

// smn_passthrough's INFO MSR, if the host provides it.
inline bool smnw_info(uint64_t &info) {
  return is_amd() && processor::rdmsr_safe(smnw_msr_info, info) &&
         static_cast<uint32_t>(info) == smnw_magic &&
         ((info >> 32) & 0xFF) >= 1;
}

inline double intel_temperature(TempDomain domain) {
  // CPUID.06H:EAX[0] digital thermal sensor, [6] package thermal management.
  uint32_t a = processor::cpuid(6).a;
  bool core = domain == TempDomain::CORE;
  uint64_t target, status;
  if (!(a & (core ? 1u : 1u << 6)) ||
      !processor::rdmsr_safe(intel_msr_temperature_target, target) ||
      !processor::rdmsr_safe(core ? intel_msr_therm_status
                                  : intel_msr_package_therm_status,
                             status))
    return NAN;
  unsigned tjmax = (target >> 16) & 0xFF;
  // Only the core sensor has a "reading valid" bit.
  if (!tjmax || (core && !(status & (1ull << 31))))
    return NAN;
  return static_cast<double>(tjmax) - static_cast<double>((status >> 16) & 0x7F);
}

inline double temperature(TempDomain domain) {
  if (is_intel())
    return intel_temperature(domain);
  uint64_t info, raw;
  if (domain != TempDomain::PKG || !smnw_info(info) ||
      !processor::rdmsr_safe(smnw_msr_tctl, raw))
    return NAN;
  // THM_TCON_CUR_TMP: [31:21] in 0.125 degrees, bit 19 selects -49 degrees.
  double c = static_cast<double>((raw >> 21) & 0x7FF) * 0.125;
  return (raw & (1ull << 19)) ? c - 49.0 : c;
}

// The CCDs that have a sensor, as a bit mask (AMD only).
inline uint32_t temperature_ccd_mask() {
  uint64_t info;
  return smnw_info(info) ? static_cast<uint32_t>(info >> 48) : 0;
}

inline double temperature_ccd(unsigned ccd) {
  uint64_t raw;
  if (ccd >= 16 || !(temperature_ccd_mask() & (1u << ccd)) ||
      !processor::rdmsr_safe(smnw_msr_ccd0 + ccd, raw) || !(raw & (1u << 11)))
    return NAN;
  // [10:0] in 0.125 degrees, offset by -49 degrees; bit 11 is "valid".
  return static_cast<double>(raw & 0x7FF) * 0.125 - 49.0;
}

// x86 has no PMCEID equivalent: the catalogue is per-vendor and validated by
// construction, so there is nothing to enumerate.
inline void pmu_dump_state() {
  std::cout << "PMU: vendor=" << (is_intel() ? "intel" : is_amd() ? "amd" : "?")
            << " signature=0x" << std::hex << pmu_design_id() << std::dec
            << " counters=" << pmu_num_counters()
            << " counter-width=" << pmc_overflow_width() << std::endl;
}

} // namespace perf
