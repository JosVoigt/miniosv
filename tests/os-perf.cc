/*
 * osv/perf.hh on the hardware counters.
 *
 *   - one pinned thread per cpu, each with its own default PerfEvent, all
 *     counting at the same time, twice in a row: every event of every thread
 *     is measured (each cpu has its own PMU and its own reservation table)
 *   - a PerfEventBlock prints its report
 *   - a sampler next to a PerfEvent on the same cpu: samples are taken and
 *     the PerfEvent's events are still measured
 *   - RAPL energy: plausible where a domain exists, NaN where it doesn't
 *   - CPPC/HWP: the same work at three fixed performance levels, the clock
 *     follows them and the original request comes back
 *   - temperature: plausible where a sensor exists, NaN where it doesn't
 *   - all together: every cpu hot, then cold; clock, power and temperature
 *     have to follow
 *
 * The counter tests are skipped where the PMU is not available (TCG, a
 * hypervisor without vPMU). RAPL and CPPC need the host to pass them through
 * (dvfs_passthrough.ko), temperature on AMD needs smn_passthrough.ko, and the
 * vCPUs have to be pinned; otherwise they're n/a.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <osv/perf.hh>
#include <osv/sched.hh>

static int g_failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            g_failures++; \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static uint64_t work(uint64_t n)
{
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < n; ++i) {
        x = x + i * 3;
    }
    return x;
}

static constexpr uint64_t iterations = 50'000'000;

static void check_events(const perf::PerfEvent &e, const char *what,
                         unsigned id, int round)
{
    for (const auto &event : e.events) {
        CHECK(event.valid, "%s %u round %d: %s not measured", what, id, round,
              event.pmce.name);
        CHECK(!event.valid || event.report() > 0,
              "%s %u round %d: %s counted 0", what, id, round,
              event.pmce.name);
    }
}

static void test_parallel()
{
    unsigned n = sched::cpus.size();
    printf("  %u pinned threads, one PerfEvent each, counting at once\n", n);

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<sched::thread *> threads;
    for (unsigned i = 0; i < n; ++i) {
        threads.push_back(sched::thread::make([&, i] {
            perf::PerfEvent e;
            ready++;
            while (!go.load()) {
            }
            for (int round = 0; round < 2; ++round) {
                e.startCounters();
                work(iterations);
                e.stopCounters();
                check_events(e, "thread", i, round);
            }
        }, sched::thread::attr().pin(sched::cpus[i])));
        threads.back()->start();
    }
    while (ready.load() < n) {
    }
    go = true;
    for (auto *t : threads) {
        t->join();
        delete t;
    }
}

static void test_block()
{
    printf("  PerfEventBlock report\n");
    perf::BenchmarkParameters params("os-perf");
    params.setParam("elements", iterations);
    perf::PerfEventBlock block(iterations, params);
    work(iterations);
}

static void test_sampler_with_event()
{
    printf("  sampler next to a PerfEvent on cpu 0\n");
    static std::atomic<uint64_t> samples;
    samples = 0;
    bool started = false;
    auto *t = sched::thread::make([&] {
        perf::PerfEvent e;
        e.startCounters();
        perf::PerfSampler sampler(1'000'000,
                                  [](exception_frame *) { samples++; });
        started = sampler.start();
        work(iterations);
        sampler.stop();
        e.stopCounters();
        check_events(e, "sampler cpu", 0, 0);
    }, sched::thread::attr().pin(sched::cpus[0]));
    t->start();
    t->join();
    delete t;
    CHECK(started, "sampler did not start");
    CHECK(samples.load() > 0, "sampler took no samples");
    printf("    %lu samples\n", static_cast<unsigned long>(samples.load()));
}

static const char *domain_name(perf::EnergyDomain d)
{
    switch (d) {
    case perf::EnergyDomain::PKG: return "pkg";
    case perf::EnergyDomain::CORE: return "core";
    case perf::EnergyDomain::CORES: return "cores";
    case perf::EnergyDomain::DRAM: return "dram";
    }
    return "?";
}

static void test_energy()
{
    printf("  RAPL energy\n");
    auto *t = sched::thread::make([] {
        perf::PerfEvent e(false);
        e.startCounters();
        work(iterations * 4);
        e.stopCounters();
        for (auto d : {perf::EnergyDomain::PKG, perf::EnergyDomain::CORE,
                       perf::EnergyDomain::CORES, perf::EnergyDomain::DRAM}) {
            double j = e.getEnergy(d);
            if (!perf::rapl_available(d)) {
                CHECK(std::isnan(j), "%s: not available but reads %f J",
                      domain_name(d), j);
                printf("    %-5s n/a\n", domain_name(d));
                continue;
            }
            // Anything from an idle core to a server package.
            double w = e.getPower(d);
            CHECK(j > 0, "%s: %f J", domain_name(d), j);
            CHECK(w > 0.01 && w < 1000, "%s: %f W is implausible",
                  domain_name(d), w);
            printf("    %-5s %.6f J  %.2f W\n", domain_name(d), j, w);
        }
    }, sched::thread::attr().pin(sched::cpus[0]));
    t->start();
    t->join();
    delete t;
}

// One cpu busy, the others asleep, each reading its own core's counter over
// the same window, with every cpu taking a turn at being busy. The busy cpu
// has to read the most. The guest can't see the host's topology: vCPUs on SMT
// siblings share a core and read the same, those on other cores far less.
static void test_core_energy()
{
    unsigned n = sched::cpus.size();
    if (n < 2 || !perf::rapl_available(perf::EnergyDomain::CORE)) {
        printf("  per-core energy: n/a\n");
        return;
    }
    printf("  per-core energy, one cpu busy at a time [J]\n");
    printf("    busy");
    for (unsigned i = 0; i < n; ++i)
        printf("  %9s%u", "cpu", i);
    printf("\n");

    constexpr auto window = std::chrono::milliseconds(300);
    for (unsigned busy = 0; busy < n; ++busy) {
        std::vector<double> joules(n);
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<sched::thread *> threads;
        for (unsigned i = 0; i < n; ++i) {
            threads.push_back(sched::thread::make([&, i] {
                perf::EnergyEvent e(perf::EnergyDomain::CORE, "core");
                ready++;
                while (!go.load()) {
                }
                auto end = std::chrono::steady_clock::now() + window;
                e.start();
                if (i == busy) {
                    while (std::chrono::steady_clock::now() < end)
                        work(100'000);
                } else {
                    sched::thread::sleep(window);
                }
                e.stop();
                joules[i] = e.report();
            }, sched::thread::attr().pin(sched::cpus[i])));
            threads.back()->start();
        }
        while (ready.load() < n) {
        }
        go = true;
        for (auto *t : threads) {
            t->join();
            delete t;
        }

        printf("    %4u", busy);
        double max = 0;
        for (unsigned i = 0; i < n; ++i) {
            printf("  %10.6f", joules[i]);
            CHECK(joules[i] >= 0, "busy %u: cpu %u reads %f J", busy, i,
                  joules[i]);
            max = std::max(max, joules[i]);
        }
        printf("\n");
        CHECK(joules[busy] >= max * 0.95,
              "busy cpu %u reads %f J, an idle one %f J", busy, joules[busy],
              max);
    }
}

// Same work at fixed performance levels: the clock has to follow the
// request, and the original request has to come back afterwards.
static void test_cppc()
{
    printf("  CPPC/HWP requests\n");
    auto caps = perf::cppc_caps();
    if (!caps && perf::cppc_available()) {
        perf::CppcCaps raw{};
        perf::cppc_read_caps(raw);
        printf("    skipped: the capabilities make no sense (highest=%u "
               "nominal=%u lowest_nonlinear=%u lowest=%u)\n", raw.highest,
               raw.nominal, raw.lowest_nonlinear, raw.lowest);
        return;
    }
    if (!caps) {
        CHECK(!perf::cppc_request() && !perf::cppc_set_request({}),
              "CPPC not available, but requests work");
        printf("    n/a\n");
        return;
    }
    printf("    caps: highest=%u nominal=%u lowest_nonlinear=%u lowest=%u\n",
           caps->highest, caps->nominal, caps->lowest_nonlinear, caps->lowest);

    auto *t = sched::thread::make([&] {
        auto before = perf::cppc_request();
        CHECK(before.has_value(), "can't read the request");

        uint8_t levels[] = {caps->lowest_nonlinear, caps->nominal,
                            caps->highest};
        double ghz[3] = {};
        printf("    %5s %8s %8s %12s %8s\n", "level", "GHz", "sec", "pkg-J",
               "pkg-W");
        for (int i = 0; i < 3; ++i) {
            perf::CppcScope scope(levels[i]);
            CHECK(scope.ok(), "level %u not applied", levels[i]);
            auto now = perf::cppc_request();
            CHECK(now && now->desired == levels[i] && now->min == levels[i] &&
                  now->max == levels[i], "level %u doesn't read back",
                  levels[i]);
            work(iterations / 10); // let the clock settle
            perf::PerfEvent e;
            e.startCounters();
            work(iterations * 2);
            e.stopCounters();
            ghz[i] = e.getCounter(perf::PERF_COUNT_HW::CPU_CYCLES.name) /
                     e.getDuration() / 1e9;
            printf("    %5u %8.2f %8.3f %12.6f %8.2f\n", levels[i], ghz[i],
                   e.getDuration(), e.getEnergy(perf::EnergyDomain::PKG),
                   e.getPower(perf::EnergyDomain::PKG));
        }
        // Loose: boost and thermals move the top, but not by this much.
        CHECK(ghz[2] > ghz[0] * 1.3,
              "%.2f GHz at the highest level vs %.2f GHz at the lowest "
              "nonlinear one", ghz[2], ghz[0]);

        auto after = perf::cppc_request();
        CHECK(after && before && *after == *before, "request not restored");
    }, sched::thread::attr().pin(sched::cpus[0]));
    t->start();
    t->join();
    delete t;
}

static bool plausible_temp(double c)
{
    return c > 0 && c < 120;
}

static void test_temperature()
{
    printf("  temperature\n");
    for (auto d : {perf::TempDomain::PKG, perf::TempDomain::CORE}) {
        const char *name = d == perf::TempDomain::PKG ? "pkg" : "core";
        double c = perf::temperature(d);
        if (std::isnan(c)) {
            printf("    %-6s n/a\n", name);
            continue;
        }
        CHECK(plausible_temp(c), "%s: %.3f C", name, c);
        printf("    %-6s %.3f C\n", name, c);
    }
    uint32_t mask = perf::temperature_ccd_mask();
    for (unsigned i = 0; i < 16; ++i) {
        double c = perf::temperature_ccd(i);
        if (!(mask & (1u << i))) {
            CHECK(std::isnan(c), "ccd %u: not present but reads %.3f C", i, c);
            continue;
        }
        CHECK(plausible_temp(c), "ccd %u: %.3f C", i, c);
        printf("    ccd%-3u %.3f C\n", i, c);
    }
    if (!mask)
        printf("    ccds   n/a\n");
}

// Everything at once: every cpu runs the same work at one fixed CPPC level,
// first the highest, then the lowest nonlinear one. Cpu 0 measures the
// clock, the package energy and the temperature along the way. Hot has to
// clock higher, draw more power and end up warmer than cold.
static void test_all_together()
{
    using clock = std::chrono::steady_clock;
    constexpr auto phase = std::chrono::seconds(3);
    constexpr auto sample = std::chrono::milliseconds(500);

    auto caps = perf::cppc_caps();
    if (!caps || !perf::rapl_available(perf::EnergyDomain::PKG) ||
        std::isnan(perf::temperature(perf::TempDomain::PKG))) {
        printf("  RAPL + CPPC + temperature together: n/a\n");
        return;
    }
    unsigned n = sched::cpus.size();
    printf("  RAPL + CPPC + temperature together, %u cpus, %lld s per level\n",
           n, static_cast<long long>(phase.count()));

    struct result {
        double ghz, watts, t_start, t_end;
    };
    auto run = [&](uint8_t level) {
        result r{};
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<sched::thread *> threads;
        printf("    level %3u, Tctl:", level);
        for (unsigned i = 0; i < n; ++i) {
            threads.push_back(sched::thread::make([&, i] {
                perf::CppcScope scope(level);
                CHECK(scope.ok(), "cpu %u: level %u not applied", i, level);
                ready++;
                while (!go.load()) {
                }
                auto end = clock::now() + phase;
                if (i) {
                    while (clock::now() < end)
                        work(100'000);
                    return;
                }
                perf::PerfEvent e;
                r.t_start = perf::temperature(perf::TempDomain::PKG);
                printf(" %.1f", r.t_start);
                e.startCounters();
                for (auto next = clock::now() + sample; clock::now() < end;) {
                    work(100'000);
                    if (clock::now() >= next) {
                        printf(" %.1f", perf::temperature(perf::TempDomain::PKG));
                        next += sample;
                    }
                }
                e.stopCounters();
                r.t_end = perf::temperature(perf::TempDomain::PKG);
                r.ghz = e.getCounter(perf::PERF_COUNT_HW::CPU_CYCLES.name) /
                        e.getDuration() / 1e9;
                r.watts = e.getPower(perf::EnergyDomain::PKG);
            }, sched::thread::attr().pin(sched::cpus[i])));
            threads.back()->start();
        }
        while (ready.load() < n) {
        }
        go = true;
        for (auto *t : threads) {
            t->join();
            delete t;
        }
        printf(" C\n      %.2f GHz, %.2f W, %.1f -> %.1f C\n", r.ghz, r.watts,
               r.t_start, r.t_end);
        return r;
    };

    auto hot = run(caps->highest);
    auto cold = run(caps->lowest_nonlinear);
    CHECK(hot.ghz > cold.ghz * 1.3, "hot %.2f GHz vs cold %.2f GHz", hot.ghz,
          cold.ghz);
    CHECK(hot.watts > cold.watts, "hot %.2f W vs cold %.2f W", hot.watts,
          cold.watts);
    CHECK(hot.t_end > cold.t_end + 1, "hot ends at %.1f C, cold at %.1f C",
          hot.t_end, cold.t_end);
}

int os_perf_main()
{
    printf("---- perf counters ----\n");
    g_failures = 0;

    perf::pmu_dump_state();
    // RAPL needs no PMU, and must read NaN without passthrough.
    test_energy();
    test_core_energy();
    test_temperature();
    if (perf::pmu_num_counters() == 0) {
        printf("---- perf counters: skipped (no PMU) ----\n");
        return g_failures ? 1 : 0;
    }

    test_parallel();
    test_block();
    test_sampler_with_event();
    test_cppc();
    test_all_together();

    printf("---- perf counters: %s ----\n",
           g_failures ? "FAILURE" : "all checks passed");
    return g_failures ? 1 : 0;
}
