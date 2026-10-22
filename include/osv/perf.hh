#pragma once

// Perf-counter front-end. Targets recent chips only; no backward compat.
// Tested: x86 = AMD Zen 4/5, Intel Skylake;
//         ARM = Ampere-1a, Neoverse V1/V2, Cortex-A76/A55.
// Arch-specific PMU access lives in arch/$(arch)/arch-perf.hh.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <osv/migration-lock.hh>
#include <osv/sched.hh>

namespace perf {

// Class of PMC. ARM has a dedicated cycle counter register; x86 only has CORE.
enum class PMClass { CORE, CYCLES };

// Event descriptor: bitmap goes into the counter's event-select register.
struct PMCEvent {
  uint64_t bitmap;
  PMClass pmClass;
  const char *name;
};

// Neoverse V1 MIDR (Implementer=0x41, Arch=0xF, Part=0xD40). Front-end owns
// this because it's used by the arch-neutral PMCSelectCore workaround.
inline constexpr uint32_t midr_neoverse_v1 = 0x410F'D400u;

// RAPL energy domains. The vendors don't line up, so each domain means one
// thing and reads NaN where it doesn't exist:
//   PKG   - the whole package (AMD, Intel)
//   CORE  - the physical core of the calling cpu (AMD only)
//   CORES - all cores of the package together, Intel's PP0 (Intel only)
//   DRAM  - memory attached to the package (Intel, where present)
// Under KVM nothing is virtualized: PKG includes the host and other VMs, and
// CORE is that of the physical cpu the vCPU runs on.
enum class EnergyDomain { PKG, CORE, CORES, DRAM };

// CPPC abstract performance levels (0-255), as in ACPI CPPC. Intel's HWP
// levels map onto them: guaranteed -> nominal, most efficient ->
// lowest_nonlinear.
struct CppcCaps {
  uint8_t highest;
  uint8_t nominal;
  uint8_t lowest_nonlinear;
  uint8_t lowest;
};

// A CPPC performance request. desired = 0 lets the hardware pick between min
// and max, guided by epp (0 = performance, 255 = energy saving).
struct CppcRequest {
  uint8_t max;
  uint8_t min;
  uint8_t desired;
  uint8_t epp;

  bool operator==(const CppcRequest &) const = default;
};

// Temperature sensors, read as absolute degrees Celsius; NaN where the sensor
// doesn't exist. As with energy, the vendors don't line up:
//   PKG  - AMD Tctl, Intel package temperature
//   CORE - the physical core of the calling cpu (Intel only; AMD has no
//          per-core sensors, only per CCD, see temperature_ccd())
// A guest only has them when the host provides them: smn_passthrough.ko on
// AMD, thermal MSR passthrough on Intel.
enum class TempDomain { PKG, CORE };

} // namespace perf

// Arch back-end provides:
//   - pmc_read / pmc_write_counter / pmc_start_with_conf / pmc_stop
//   - enable_pmu, pmu_num_counters, pmc_overflow_width, pmu_design_id, is_midr
//   - x86 only: cpu_vendor, is_intel, is_amd
//   - namespace perf::PERF_COUNT_HW event catalogue
//   - rapl_available, rapl_energy_unit, rapl_read, rapl_counter_width
//   - cppc_available, cppc_read_caps, cppc_read_request, cppc_write_request
//     (CPPC on AMD, HWP on Intel; stubs reporting "not available" on ARM)
//   - temperature, temperature_ccd_mask, temperature_ccd (NaN / 0 on ARM)
#include <arch-perf.hh>

namespace perf {

// Physical counter + its config register.
struct PMC {
  // Event-select register; on ARM also indexes pmevtyperN_el0.
  uint32_t perfEvtSel;
  // Counter-value register.
  uint32_t perfCtr;
  PMClass pmClass;
  // True when the counter is available for reservation.
  mutable std::atomic<bool> free{true};
  // Times this counter has overflowed since wrap counting was enabled. Only
  // meaningful when the selection is counting wraps; see enable_wrap_counting.
  mutable std::atomic<uint64_t> wraps{0};

  PMC(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass)
      : perfEvtSel(perfEvtSel), perfCtr(perfCtr), pmClass(pmClass) {}

  // std::atomic is not copyable; provide copy semantics manually.
  PMC(const PMC &pmc)
      : perfEvtSel(pmc.perfEvtSel), perfCtr(pmc.perfCtr), pmClass(pmc.pmClass) {
  }

  PMC &operator=(const PMC &pmc) {
    perfEvtSel = pmc.perfEvtSel;
    perfCtr = pmc.perfCtr;
    pmClass = pmc.pmClass;
    free.store(true);
    return *this;
  }

  uint64_t read() const { return pmc_read(perfCtr); }

  bool start_with_conf(uint64_t value, uint64_t initial = 0) {
    pmc_write_counter(perfCtr, initial);
    return pmc_start_with_conf(perfCtr, perfEvtSel, value);
  }

  void stop() { pmc_stop(perfEvtSel); }
};

// A set of counters to reserve from. The hardware counters are per cpu, so a
// PMCSelect describes the counters of one cpu; PerCpuPMCSelect below picks the
// calling cpu's one.
struct PMCSelect {
  explicit PMCSelect(std::vector<PMC> pmcs)
      : design_id(pmu_design_id()), pmcs(std::move(pmcs)) {}
  virtual ~PMCSelect() = default;

  bool erase_counter(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass) {
    auto it = std::find_if(pmcs.begin(), pmcs.end(), [&](const auto &pmc) {
      return pmc.perfEvtSel == perfEvtSel && pmc.perfCtr == perfCtr &&
             pmc.pmClass == pmClass;
    });
    if (it == pmcs.end())
      return false;
    pmcs.erase(it);
    return true;
  }

  bool erase_last_n_of_x(uint32_t n, PMClass x) {
    auto it = pmcs.end();
    while (n > 0 && it != pmcs.begin()) {
      --it;
      if (it->pmClass == x) {
        it = pmcs.erase(it);
        --n;
      }
    }
    return n == 0;
  }

  // Try to reserve any free PMC of the given class. Retries a bounded number
  // of times to tolerate transient contention with another thread.
  virtual PMC *acquire(PMClass cls) {
    constexpr int max_retries = 7;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
      for (auto &pmc : pmcs) {
        bool expected = true;
        if (pmc.pmClass == cls &&
            pmc.free.compare_exchange_strong(expected, false))
          return &pmc;
      }
    }
    return nullptr;
  }

  virtual void release(PMC *pmc) { pmc->free.store(true); }

  // The counter is one of this selection's.
  bool owns(const PMC *pmc) const {
    return !pmcs.empty() && pmc >= pmcs.data() &&
           pmc < pmcs.data() + pmcs.size();
  }

  // Software-extend the hardware counters to 64 bits. An Armv8 PMU without
  // FEAT_PMUv3p5 has 32-bit event counters, which wrap after a couple of
  // seconds of any event that tracks the clock. No-op where the hardware
  // already counts wide enough.
  //
  // The PMU is per-cpu and so is this handler: call it on the cpu whose
  // counters this selection describes, which also enables the PMU interrupt
  // there. The totals are only right if the measured thread stays on one cpu.
  virtual void enable_wrap_counting() {
    if (counting_wraps.load(std::memory_order_acquire))
      return;
    // Two threads can start counters at once; only one handler may be
    // attached. counting_wraps is published last, so arm_wrap_counting never
    // runs before wrap_irq is set.
    std::lock_guard<std::mutex> guard(wrap_lock);
    if (counting_wraps.load(std::memory_order_relaxed))
      return;
    if (pmc_overflow_width(0) > 32)
      return;
    unsigned cpu = sched::cpu::current()->id;
    wrap_irq = pmc_attach_overflow_handler([this, cpu] {
      // One PMU interrupt is shared by every handler: the other cpus' tables
      // and any PMCSampler are attached to it too. Touch only the counters
      // this table armed on its own cpu, and ack only those -- acking the
      // whole status word would swallow a sampler's overflow.
      if (sched::cpu::current()->id != cpu)
        return;
      uint64_t mine =
          pmu_overflow_status() & armed_mask.load(std::memory_order_relaxed);
      if (!mine)
        return;
      for (auto &pmc : pmcs) {
        if (mine & pmc_overflow_bit(pmc.perfCtr))
          pmc.wraps.fetch_add(1, std::memory_order_relaxed);
      }
      pmc_ack_overflow_mask(mine, wrap_irq);
    });
    counting_wraps.store(true, std::memory_order_release);
  }

  virtual bool counts_wraps() const {
    return counting_wraps.load(std::memory_order_acquire);
  }

  // Arm the overflow interrupt for one counter. Called once the counter is
  // running and zeroed, because arming it earlier would count wraps nobody is
  // measuring. The flag left behind by the counter's previous owner has to go
  // first: the PMU interrupt is level-triggered, so arming on top of a stale
  // flag keeps it asserted.
  virtual void arm_wrap_counting(PMC *pmc) {
    if (!counts_wraps())
      return;
    uint64_t bit = pmc_overflow_bit(pmc->perfCtr);
    pmc_ack_overflow_mask(bit, wrap_irq);
    armed_mask.fetch_or(bit, std::memory_order_relaxed);
    pmc_overflow_ack_conf(pmc->perfCtr);
  }

  // Stop taking interrupts for a counter nobody is measuring any more.
  virtual void disarm_wrap_counting(PMC *pmc) {
    if (!counts_wraps())
      return;
    uint64_t bit = pmc_overflow_bit(pmc->perfCtr);
    armed_mask.fetch_and(~bit, std::memory_order_relaxed);
    pmc_disable_overflow_int(bit);
  }

  virtual size_t size() const { return pmcs.size(); }

  virtual size_t size_of_x(PMClass x) const {
    size_t n = 0;
    for (const auto &pmc : pmcs)
      if (pmc.pmClass == x)
        ++n;
    return n;
  }

protected:
  // MIDR (ARM) or CPUID signature (x86) of the cpu this table was built on.
  // Every cpu has its own table, so this is per cpu as well.
  uint32_t design_id;
  std::vector<PMC> pmcs;

private:
  std::mutex wrap_lock;
  PMCIntHandle wrap_irq{};
  std::atomic<bool> counting_wraps{false};
  // Counters currently armed for wrap counting, as PMOVSCLR bits.
  std::atomic<uint64_t> armed_mask{0};
};

// Core-local counter selection. Adjusts the counter count at runtime because
// AWS slices the number of counters per VM.
struct PMCSelectCore : PMCSelect {
  explicit PMCSelectCore(std::vector<PMC> pmcs) : PMCSelect(std::move(pmcs)) {
    uint32_t act_ctrs = pmu_num_counters();
    uint32_t exp_ctrs = 0;
    for (const auto &c : this->pmcs)
      if (c.pmClass == PMClass::CORE)
        ++exp_ctrs;

    if (act_ctrs < exp_ctrs) {
      std::cout << "Expected " << exp_ctrs << " hardware counters, but only "
                << act_ctrs << " are available.\n Assuming the first "
                << act_ctrs << " counters to be valid." << std::endl;
      erase_last_n_of_x(exp_ctrs - act_ctrs, PMClass::CORE);
    }
    // Independent of the trimming above: a VM with sliced counters can run on
    // a Neoverse V1 as well.
    if (is_midr(midr_neoverse_v1) && is_kvm_guest() &&
        erase_counter(0, 0, PMClass::CORE)) {
      std::cout << "Detected ARM Neoverse V1 under KVM: disabling counter 0 "
                   "since it doesn't work reliably in this configuration. "
                   "You have "
                << size_of_x(PMClass::CORE) << " counters available"
                << std::endl;
    }
  }
};

// Default core-local counter list. Counter MSRs are vendor-specific on x86
// (resolved at runtime via CPUID); ARM uses fixed indices.
inline std::vector<PMC> make_default_core_pmcs() {
  std::vector<PMC> pmcs;
#if defined(__x86_64__)
  uint32_t n = pmu_num_counters();
  if (is_intel()) {
    // IA32_A_PMCx when the CPU supports full-width counter writes.
    uint32_t ctr = intel_full_width_write() ? 0x4C1u : 0xC1u;
    for (uint32_t i = 0; i < n; ++i)
      pmcs.emplace_back(0x186u + i, ctr + i, PMClass::CORE);
  } else {
    // AMD "extended" core PMC range (Zen and later): counters live at
    // MSRC001_0200h + 2n / MSRC001_0201h + 2n.
    for (uint32_t i = 0; i < n; ++i)
      pmcs.emplace_back(0xC0010200u + 2 * i, 0xC0010201u + 2 * i,
                        PMClass::CORE);
  }
#elif defined(__aarch64__)
  // Armv8 PMUv3 usually has 6 counters (ids 0-5).
  for (uint32_t i = 0; i < 6; ++i)
    pmcs.emplace_back(i, i, PMClass::CORE);
  pmcs.emplace_back(1u << 31, 1u << 31, PMClass::CYCLES);
#endif
  return pmcs;
}

// The counters of the calling cpu. Every cpu has its own PMU, so every cpu
// gets its own reservation table (a PMCSelectCore), created on that cpu the
// first time it is used there. It reads that cpu's PMU, so the clusters of an
// asymmetric part (Cortex-A76 + A55, x86 hybrids) get tables that match their
// counter counts. Reserving on one cpu does not take counters from another;
// two users on the same cpu share its table and cannot program the same
// counter twice.
//
// acquire() reserves on the calling cpu; release() and the wrap counting go
// to the table the counter came from. With pinned threads each thread
// reserves on its own cpu.
struct PerCpuPMCSelect : PMCSelect {
  PerCpuPMCSelect() : PMCSelect({}), selections(sched::cpus.size()) {}

  PMC *acquire(PMClass cls) override { return local().acquire(cls); }

  void release(PMC *pmc) override {
    if (PMCSelect *owner = owner_of(pmc))
      owner->release(pmc);
  }

  void enable_wrap_counting() override { local().enable_wrap_counting(); }

  bool counts_wraps() const override { return local().counts_wraps(); }

  void arm_wrap_counting(PMC *pmc) override {
    if (PMCSelect *owner = owner_of(pmc))
      owner->arm_wrap_counting(pmc);
  }

  void disarm_wrap_counting(PMC *pmc) override {
    if (PMCSelect *owner = owner_of(pmc))
      owner->disarm_wrap_counting(pmc);
  }

  size_t size() const override { return local().size(); }

  size_t size_of_x(PMClass x) const override { return local().size_of_x(x); }

private:
  // The calling cpu's table, created on first use. migration_lock keeps the
  // thread on the cpu it read its id from while the table reads that PMU.
  PMCSelectCore &local() const {
    std::lock_guard<migration_lock_t> stay(migration_lock);
    auto &slot = selections[sched::cpu::current()->id];
    PMCSelectCore *selection = slot.load(std::memory_order_acquire);
    if (selection)
      return *selection;
    auto *created = new PMCSelectCore(make_default_core_pmcs());
    if (slot.compare_exchange_strong(selection, created,
                                     std::memory_order_acq_rel))
      return *created;
    delete created; // another thread on this cpu was faster
    return *selection;
  }

  PMCSelect *owner_of(const PMC *pmc) const {
    for (auto &slot : selections) {
      PMCSelectCore *selection = slot.load(std::memory_order_acquire);
      if (selection && selection->owns(pmc))
        return selection;
    }
    return nullptr;
  }

  // One table per cpu; never freed (the wrap counting handlers refer to them).
  mutable std::vector<std::atomic<PMCSelectCore *>> selections;
};

// The machine-wide default: the calling cpu's counters, see PerCpuPMCSelect.
inline PMCSelect &default_core_pmcs() {
  static PerCpuPMCSelect selection;
  return selection;
}

// ---------------- High-level measurement API ----------------

struct Event {
  PMCEvent pmce;

  uint64_t before;
  uint64_t after;

  bool valid = true;

  Event(PMCEvent pmce, PMCSelect &pmcs) : pmce(pmce), pmcs(pmcs) {}

  void start() {
    pmc = pmcs.acquire(pmce.pmClass);
    if (!pmc) {
      std::cerr << "[ERROR] All hardware counters are occupied ("
                << pmcs.size_of_x(pmce.pmClass) << "/"
                << pmcs.size_of_x(pmce.pmClass) << "). Event " << pmce.name
                << " will not be measured." << std::endl;
      valid = false;
      return;
    }
    width = pmc_overflow_width(pmc->perfCtr);
    polled = 0;
    if (!pmc->start_with_conf(pmce.bitmap)) {
      // The event is not implemented here; the counter stays disabled and
      // would otherwise report a plausible-looking zero.
      pmcs.release(pmc);
      pmc = nullptr;
      valid = false;
      return;
    }
    // Armed only now that start_with_conf has zeroed the counter: arming it
    // beforehand would fold in a wrap of whatever the previous owner left.
    wraps_before = pmc->wraps.load(std::memory_order_relaxed);
    pmcs.arm_wrap_counting(pmc);
    before = last = pmc->read();
  }

  // Read the counter and fold in an overflow if it has gone backwards. Cheap
  // enough to call from a timer (one system-register read per counter). Must
  // be called more often than the counter can wrap -- on a 32-bit PMU counting
  // cycles, that is every couple of seconds.
  void poll() {
    if (!pmc || width >= 64)
      return;
    uint64_t now = pmc->read();
    if (now < last)
      polled += 1ull << width;
    last = now;
  }

  void stop() {
    if (!pmc)
      return;
    poll();
    after = pmc->read();
    if (width < 64 && after < last)
      polled += 1ull << width;
    pmc->stop();
    pmcs.disarm_wrap_counting(pmc);
    wraps = pmc->wraps.load(std::memory_order_relaxed) - wraps_before;
    counted_wraps = pmcs.counts_wraps();
    pmcs.release(pmc);
    // With the overflows added back a counter only ever counts up, so this is
    // not one: it is a read of a different cpu's PMU, or of a counter that was
    // never enabled.
    if (!counted_wraps && !polled && after < before)
      valid = false;
  }

  uint64_t report() const {
    if (!valid)
      return 0;
    // Overflows come from the interrupt where KVM delivers it and from polling
    // otherwise; the two are alternatives, so taking the larger picks whichever
    // was actually working. A counter that is already 64 bits wide cannot wrap,
    // and shifting by its full width would be undefined.
    uint64_t wrapped = width < 64 ? wraps << width : 0;
    uint64_t overflowed = std::max(polled, wrapped);
    return overflowed + after - before;
  }

private:
  PMCSelect &pmcs;
  PMC *pmc = nullptr;
  uint64_t wraps_before = 0;
  uint64_t wraps = 0;
  uint64_t last = 0;
  uint64_t polled = 0;
  uint32_t width = 64;
  bool counted_wraps = false;
};

// Energy used in one RAPL domain between start() and stop(), in joules; NaN
// where the domain doesn't exist (see EnergyDomain). Unlike an Event it takes
// no counter: the RAPL counters always run. They are 32 bits wide and wrap
// after tens of kJ (about an hour at 15 W in the usual 15.3 uJ unit); stop()
// catches one wrap, poll() more often than that catches the rest.
struct EnergyEvent {
  EnergyDomain domain;
  const char *name;

  EnergyEvent(EnergyDomain domain, const char *name)
      : domain(domain), name(name) {}

  void start() {
    valid = rapl_available(domain) && rapl_read(domain, before);
    if (!valid)
      return;
    unit = rapl_energy_unit(domain);
    cpu = sched::cpu::current()->id;
    last = before;
    wraps = 0;
  }

  void poll() {
    uint64_t now;
    if (!valid)
      return;
    // A per-core counter read on another cpu is another core's.
    if (domain == EnergyDomain::CORE && sched::cpu::current()->id != cpu) {
      valid = false;
      return;
    }
    if (!rapl_read(domain, now))
      return;
    if (now < last)
      ++wraps;
    last = now;
  }

  void stop() {
    poll();
    after = last;
  }

  double report() const {
    if (!valid)
      return NAN;
    return static_cast<double>((wraps << rapl_counter_width) + after - before) *
           unit;
  }

private:
  bool valid = false;
  double unit = NAN;
  unsigned cpu = 0;
  uint64_t before = 0;
  uint64_t after = 0;
  uint64_t last = 0;
  uint64_t wraps = 0;
};

// ---------------- DVFS: CPPC (AMD) / HWP (Intel) ----------------
//
// Performance requests of the calling cpu. Under KVM they are those of the
// physical cpu the vCPU runs on, so vCPUs have to be pinned 1:1 for this to
// mean anything, and a request stays in effect there after the vCPU stops
// until the host restores its own (dvfs_passthrough: enable=0 or unload).

// Nullopt also when the capabilities make no sense: firmware can report all
// zeroes (seen on a laptop running on battery), and a request built from
// those would ask for nothing at all.
inline std::optional<CppcCaps> cppc_caps() {
  CppcCaps caps;
  if (!cppc_available() || !cppc_read_caps(caps))
    return std::nullopt;
  if (!caps.highest || caps.lowest > caps.lowest_nonlinear ||
      caps.lowest_nonlinear > caps.nominal || caps.nominal > caps.highest)
    return std::nullopt;
  return caps;
}

inline std::optional<CppcRequest> cppc_request() {
  CppcRequest req;
  if (!cppc_available() || !cppc_read_request(req))
    return std::nullopt;
  return req;
}

inline bool cppc_set_request(const CppcRequest &req) {
  return cppc_available() && cppc_write_request(req);
}

// Holds a request for its lifetime and puts the previous one back. Keeps the
// thread on its cpu meanwhile, because the request belongs to that cpu.
struct CppcScope {
  explicit CppcScope(const CppcRequest &req) { apply(req, false); }

  // Run at a fixed level: min = max = desired, EPP left as it is.
  explicit CppcScope(uint8_t level) {
    apply({level, level, level, 0}, true);
  }

  ~CppcScope() {
    if (active)
      cppc_set_request(saved);
    sched::migrate_enable();
  }

  CppcScope(const CppcScope &) = delete;
  CppcScope &operator=(const CppcScope &) = delete;

  // The request was applied.
  bool ok() const { return active; }

private:
  void apply(CppcRequest req, bool keep_epp) {
    sched::migrate_disable();
    if (auto old = cppc_request()) {
      saved = *old;
      if (keep_epp)
        req.epp = old->epp;
      active = cppc_set_request(req);
    }
  }

  CppcRequest saved{};
  bool active = false;
};

struct PerfEvent {
  // The machine-wide selection unless an external one is supplied. Sharing a
  // PMCSelect between PerfEvents lets multiple collections coordinate uncore
  // counters.
  PMCSelect &pmcs = default_core_pmcs();
  // Must not exceed the number of hardware counters in `pmcs`.
  std::vector<Event> events;
  // Always all domains, so that reports have the same columns everywhere;
  // the ones this machine doesn't have read NaN.
  std::vector<EnergyEvent> energy{{EnergyDomain::PKG, "energy-pkg-J"},
                                  {EnergyDomain::CORE, "energy-core-J"},
                                  {EnergyDomain::CORES, "energy-cores-J"},
                                  {EnergyDomain::DRAM, "energy-dram-J"}};

  std::chrono::time_point<std::chrono::steady_clock> startTime;
  std::chrono::time_point<std::chrono::steady_clock> stopTime;

  // Uses this instance's own core-local counter set.
  PerfEvent(bool set_default_counters = true) {
    enable_pmu();

    if (set_default_counters) {
      registerCounter(PERF_COUNT_HW::CPU_CYCLES);
      registerCounter(PERF_COUNT_HW::INSTRUCTIONS);
      registerCounter(PERF_COUNT_HW::L2D_CACHE_MISS);
      registerCounter(PERF_COUNT_HW::BRANCH_MISS);
    }
  }

  // Shares a counter selection with other PerfEvents.
  PerfEvent(PMCSelect &pmcSelect) : pmcs(pmcSelect) { enable_pmu(); }

  void registerCounter(PMCEvent pmce) { events.emplace_back(pmce, pmcs); }

  void registerCounter(PMCEvent pmce, const char *name) {
    pmce.name = name;
    registerCounter(pmce);
  }

  void registerCounter(uint64_t bitmap, PMClass pmClass, const char *name) {
    registerCounter({bitmap, pmClass, name});
  }

  void startCounters() {
    pmcs.enable_wrap_counting();
    for (auto &event : events)
      event.start();
    for (auto &e : energy)
      e.start();
    startTime = std::chrono::steady_clock::now();
  }

  // Fold in any counter that has overflowed since the last call. Only needed
  // where the PMU overflow interrupt does not reach the guest -- report() takes
  // whichever of the two mechanisms saw more wraps -- but on a 32-bit PMU it is
  // the only thing standing between a long run and a wrong answer. Call it from
  // a timer, more often than the counter can wrap; see Event::poll.
  void pollCounters() {
    for (auto &event : events)
      event.poll();
    for (auto &e : energy)
      e.poll();
  }

  void stopCounters() {
    stopTime = std::chrono::steady_clock::now();
    for (auto &e : energy)
      e.stop();
    for (auto &event : events)
      event.stop();
  }

  double getDuration() const {
    return std::chrono::duration<double>(stopTime - startTime).count();
  }

  size_t getDurationUs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(stopTime -
                                                                 startTime)
        .count();
  }

  // Returns NaN if either instructions or cycles isn't being counted.
  double getIPC() const {
    double res = getCounter(PERF_COUNT_HW::INSTRUCTIONS.name) /
                 getCounter(PERF_COUNT_HW::CPU_CYCLES.name);
    return res > 0 ? res : NAN;
  }

  double getCounter(const char *name) const {
    for (const auto &event : events)
      if (event.pmce.name == name)
        return event.report();
    return -1;
  }

  // Joules, NaN where the domain doesn't exist.
  double getEnergy(EnergyDomain domain) const {
    for (const auto &e : energy)
      if (e.domain == domain)
        return e.report();
    return NAN;
  }

  // Average watts over the measurement.
  double getPower(EnergyDomain domain) const {
    return getEnergy(domain) / getDuration();
  }

  // Joules are small numbers; the default two decimals would round them away.
  static std::string formatEnergy(double joules) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(6) << joules;
    return stream.str();
  }

  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, std::string counterValue,
                           bool addComma = true) {
    auto width = std::max(name.length(), counterValue.length());
    headerOut << std::setw(static_cast<int>(width)) << name
              << (addComma ? "," : "") << " ";
    dataOut << std::setw(static_cast<int>(width)) << counterValue
            << (addComma ? "," : "") << " ";
  }

  template <typename T>
  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, T counterValue,
                           bool addComma = true) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    PerfEvent::printCounter(headerOut, dataOut, name, stream.str(), addComma);
  }

  void printReport(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream header;
    std::stringstream data;
    printReport(header, data, normalizationConstant);
    out << header.str() << std::endl;
    out << data.str() << std::endl;
  }

  void printReport(std::ostream &headerOut, std::ostream &dataOut,
                   uint64_t normalizationConstant) {
    if (events.empty())
      return;

    printCounter(headerOut, dataOut, "duration", getDuration());
    for (const auto &event : events) {
      printCounter(headerOut, dataOut, event.pmce.name,
                   event.report() / static_cast<double>(normalizationConstant));
    }

    // Totals: energy isn't per element like the events above.
    for (const auto &e : energy)
      printCounter(headerOut, dataOut, e.name, formatEnergy(e.report()));

    printCounter(headerOut, dataOut, "scale", normalizationConstant);

    // Derived metrics.
    printCounter(headerOut, dataOut, "IPC", getIPC());
    printCounter(headerOut, dataOut, "power-pkg-W",
                 getPower(EnergyDomain::PKG));
  }

  template <typename T>
  static void printCounterVertical(std::ostream &infoOut, std::string name,
                                   T counterValue, int eNameWidth) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    infoOut << std::setw(eNameWidth) << std::left << name << " : "
            << stream.str() << std::endl;
  }

  void printReportVertical(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream info;
    printReportVerticalUtil(info, normalizationConstant);
    out << info.str() << std::endl;
  }

  void printReportVerticalUtil(std::ostream &infoOut,
                               uint64_t normalizationConstant) {
    if (events.empty())
      return;

    // Widest event name; minimum width is that of "power-pkg-W".
    int eNameWidth = 11;
    for (const auto &event : events) {
      eNameWidth = std::max(
          static_cast<int>(std::char_traits<char>::length(event.pmce.name)),
          eNameWidth);
    }
    for (const auto &e : energy) {
      eNameWidth = std::max(
          static_cast<int>(std::char_traits<char>::length(e.name)), eNameWidth);
    }

    printCounterVertical(infoOut, "duration", getDuration(), eNameWidth);
    for (const auto &event : events) {
      printCounterVertical(infoOut, event.pmce.name,
                           event.report() /
                               static_cast<double>(normalizationConstant),
                           eNameWidth);
    }

    for (const auto &e : energy)
      printCounterVertical(infoOut, e.name, formatEnergy(e.report()),
                           eNameWidth);

    printCounterVertical(infoOut, "scale", normalizationConstant, eNameWidth);

    // Derived metrics.
    printCounterVertical(infoOut, "IPC", getIPC(), eNameWidth);
    printCounterVertical(infoOut, "power-pkg-W", getPower(EnergyDomain::PKG),
                         eNameWidth);
  }
};

struct PMCSampler {
  PMCSampler(uint64_t period, std::function<void(exception_frame *)> handler,
             PMCEvent pmce = PERF_COUNT_HW::CPU_CYCLES)
      : period(period), handler(std::move(handler)), pmce(pmce) {
    enable_pmu();
  }

  ~PMCSampler() { stop(); }

  bool start() {
    if (pmc || !(pmc = pmcs.acquire(pmce.pmClass)))
      return false;
    ack = pmc_overflow_ack_conf(pmc->perfCtr);
    // Drop a flag left by the counter's previous owner before the handler goes
    // in: the PMU interrupt is level-triggered, so a stale flag would fire the
    // handler the moment it is attached.
    pmc_clear_overflow(ack);
    vector = pmc_attach_overflow_handler([this] {
      // The interrupt is shared with the wrap-counting handler and with the
      // other clusters' tables, so it fires for overflows that are not ours.
      if (!pmc_overflow_pending(ack))
        return;
      pmc_write_counter(pmc->perfCtr, pmc_period_value(pmc->perfCtr, period));
      pmc_ack_overflow(ack, vector);
      handler(current_interrupt_frame);
    });
    if (!pmc->start_with_conf(pmce.bitmap | pmc_int_enable,
                              pmc_period_value(pmc->perfCtr, period))) {
      pmc_detach_overflow_handler(vector, ack.mask);
      pmcs.release(pmc);
      pmc = nullptr;
      return false;
    }
    return true;
  }

  void stop() {
    if (!pmc)
      return;
    pmc->stop();
    pmc_detach_overflow_handler(vector, ack.mask);
    pmcs.release(pmc);
    pmc = nullptr;
  }

private:
  PMCSelect &pmcs = default_core_pmcs();
  uint64_t period;
  std::function<void(exception_frame *)> handler;
  PMCEvent pmce;
  PMC *pmc = nullptr;
  PMCIntHandle vector{};
  PMCOverflowAck ack{};
};

struct BenchmarkParameters {

  void setParam(const std::string &name, const std::string &value) {
    params[name] = value;
  }

  void setParam(const std::string &name, const char *value) {
    params[name] = value;
  }

  template <typename T> void setParam(const std::string &name, T value) {
    setParam(name, std::to_string(value));
  }

  void printParams(std::ostream &header, std::ostream &data) {
    for (auto &p : params)
      PerfEvent::printCounter(header, data, p.first, p.second);
  }

  BenchmarkParameters(std::string name = "") {
    if (name.length())
      setParam("name", name);
  }

private:
  std::map<std::string, std::string> params;
};

struct PerfRef {
  union {
    PerfEvent instance;
    PerfEvent *pointer;
  };
  bool has_instance;

  PerfRef() : instance(), has_instance(true) {}
  PerfRef(PerfEvent *ptr) : pointer(ptr), has_instance(false) {}
  PerfRef(const PerfRef &) = delete;

  ~PerfRef() {
    if (has_instance)
      instance.~PerfEvent();
  }

  PerfEvent *operator->() { return has_instance ? &instance : pointer; }
};

struct PerfEventBlock {
  PerfRef e;
  uint64_t scale;
  BenchmarkParameters parameters;
  bool printHeader;

  PerfEventBlock(uint64_t scale = 1, BenchmarkParameters params = {},
                 bool printHeader = true)
      : scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  PerfEventBlock(PerfEvent &perf, uint64_t scale = 1,
                 BenchmarkParameters params = {}, bool printHeader = true)
      : e(&perf), scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  ~PerfEventBlock() {
    e->stopCounters();
    std::stringstream header;
    std::stringstream data;
    parameters.printParams(header, data);
    PerfEvent::printCounter(header, data, "time sec", e->getDuration());
    PerfEvent::printCounter(header, data, "micros", e->getDurationUs());
    PerfEvent::printCounter(header, data, "millis",
                            static_cast<double>(e->getDurationUs()) / 1000);
    e->printReport(header, data, scale);
    if (printHeader)
      std::cout << header.str() << std::endl;
    std::cout << data.str() << std::endl;
  }
};

} // namespace perf
