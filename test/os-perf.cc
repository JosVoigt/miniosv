/*
 * osv/perf.hh on the hardware counters.
 *
 *   - one pinned thread per cpu, each with its own default PerfEvent, all
 *     counting at the same time, twice in a row: every event of every thread
 *     is measured (each cpu has its own PMU and its own reservation table)
 *   - a PerfEventBlock prints its report
 *   - a sampler next to a PerfEvent on the same cpu: samples are taken and
 *     the PerfEvent's events are still measured
 *
 * Skipped where the PMU is not available (TCG, a hypervisor without vPMU).
 */
#include <atomic>
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
        perf::PMCSampler sampler(1'000'000,
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

int os_perf_main()
{
    printf("---- perf counters ----\n");
    g_failures = 0;

    perf::pmu_dump_state();
    if (perf::pmu_num_counters() == 0) {
        printf("---- perf counters: skipped (no PMU) ----\n");
        return 0;
    }

    test_parallel();
    test_block();
    test_sampler_with_event();

    printf("---- perf counters: %s ----\n",
           g_failures ? "FAILURE" : "all checks passed");
    return g_failures ? 1 : 0;
}
