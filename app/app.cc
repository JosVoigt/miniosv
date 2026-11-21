/*
 * LeanStore YCSB on miniOSv, as a test bed for request scheduling.
 *
 * Boot arguments (key=value, written into the image with
 * `scripts/run.py --args "..."` or scripts/setargs.py), defaults in brackets:
 *
 *   workers=4      executor cores (one pinned thread each), vCPUs first_core..
 *   first_core=1   vCPU of executor 0; the load generator runs on gen_cpu
 *   gen_cpu=0
 *   slots=1        requests a core runs at once (worker tasks per core)
 *   queue=fifo     waiting-request order     (see app/sched/Sched.cpp)
 *   place=free     which core a request lands on
 *   rate=0         offered load in requests/s (open loop, Poisson arrivals);
 *                  0 = closed loop with `inflight` requests outstanding
 *   inflight=0     request pool; 0 = 2 * workers * slots (closed loop),
 *                  65536 (open loop)
 *   run=10         measured seconds
 *   warmup=1       seconds of load before measuring
 *   read=100       % lookups, the rest are updates
 *   zipf=0         key skew (0 = uniform)
 *   data_gib=1     YCSB data set (half of it is tuple payload, as before)
 *   dram_gib=0     buffer pool; 0 = enough to hold the data set in memory
 *   io=mem         mem: no storage device, abort on any I/O; osv: NVMe
 *   load_tasks=16  tasks per worker for the insert phase
 *
 * Results go to the console as CSV lines starting with "lsched,".
 */

#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/io/IoInterface.hpp"
#include "sched/Sched.hpp"
#include "sched/Stats.hpp"
#include "Time.hpp"
#include "Units.hpp"

#include <osv/bootargs.hh>
#include <osv/sched.hh>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace leanstore;

using YCSBKey = u64;
using YCSBPayload = BytesPayload<120>;

namespace
{
// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
    int workers = 4;
    int first_core = 1;
    int gen_cpu = 0;
    int slots = 1;
    std::string queue = "fifo";
    std::string place = "free";
    double rate = 0;
    int inflight = 0;
    double run_s = 10;
    double warmup_s = 1;
    int read = 100;
    double zipf = 0;
    double data_gib = 1;
    double dram_gib = 0;
    std::string io = "mem";
    int load_tasks = 16;
};

Config parseConfig()
{
    Config c;
    std::map<std::string, std::string> kv;
    for (auto& w : osv::bootargs_split(osv::bootargs())) {
        auto eq = w.find('=');
        if (eq == std::string::npos) {
            throw std::invalid_argument("boot argument '" + w + "' is not key=value");
        }
        kv[w.substr(0, eq)] = w.substr(eq + 1);
    }
    auto take = [&](const char* k, auto& v) {
        auto it = kv.find(k);
        if (it == kv.end()) {
            return;
        }
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            v = it->second;
        } else if constexpr (std::is_same_v<T, int>) {
            v = std::stoi(it->second);
        } else {
            v = std::stod(it->second);
        }
        kv.erase(it);
    };
    take("workers", c.workers);
    take("first_core", c.first_core);
    take("gen_cpu", c.gen_cpu);
    take("slots", c.slots);
    take("queue", c.queue);
    take("place", c.place);
    take("rate", c.rate);
    take("inflight", c.inflight);
    take("run", c.run_s);
    take("warmup", c.warmup_s);
    take("read", c.read);
    take("zipf", c.zipf);
    take("data_gib", c.data_gib);
    take("dram_gib", c.dram_gib);
    take("io", c.io);
    take("load_tasks", c.load_tasks);
    if (!kv.empty()) {
        throw std::invalid_argument("unknown boot argument '" + kv.begin()->first + "'");
    }
    if (c.inflight <= 0) {
        // closed loop: keep every slot busy plus one waiting each;
        // open loop: large enough that only real overload drops requests
        c.inflight = c.rate > 0 ? 1 << 16 : 2 * c.workers * c.slots;
    }
    if (c.dram_gib <= 0) {
        // pages hold ~0.75x data_gib of tuples; leave room for inner nodes
        c.dram_gib = c.data_gib * 1.5 + 0.5;
    }
    if (c.workers < 1 || c.slots < 1) {
        throw std::invalid_argument("workers and slots must be >= 1");
    }
    const int need = std::max(c.first_core + c.workers, c.gen_cpu + 1);
    if ((size_t)need > sched::cpus.size()) {
        throw std::invalid_argument("need " + std::to_string(need) + " vCPUs, have " +
                                    std::to_string(sched::cpus.size()));
    }
    if (c.gen_cpu >= c.first_core && c.gen_cpu < c.first_core + c.workers) {
        throw std::invalid_argument("gen_cpu overlaps the worker cores");
    }
    return c;
}

// LeanStore reads these when the store and the executors are built.
void applyConfig(const Config& c)
{
    FLAGS_worker_threads = c.workers;
    FLAGS_worker_tasks = c.load_tasks;
    FLAGS_dram_gib = c.dram_gib;
    FLAGS_target_gib = c.data_gib;
    FLAGS_ycsb_read_ratio = c.read;
    FLAGS_zipf_factor = c.zipf;
    FLAGS_run_for_seconds = u64(c.run_s + c.warmup_s);
}

// Nanoseconds per TSC tick, measured against the kernel clock (Time.hpp's
// conversion assumes a fixed 4 GHz).
double nsPerTick()
{
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const u64 c0 = mean::readTSCfenced();
    while (clk::now() - t0 < std::chrono::milliseconds(100)) {
    }
    const auto t1 = clk::now();
    const u64 c1 = mean::readTSCfenced();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(c1 - c0);
}

Config cfg;
double ns_per_tick = 0;

// ---------------------------------------------------------------------------
// Load generator: runs on its own pinned kernel thread (gen_cpu), creates
// requests and hands them to the scheduler. Requests come from a fixed pool;
// slots return them after recording their latency.
// ---------------------------------------------------------------------------
struct Generator {
    lsched::Scheduler& sched;
    BTreeInterface<YCSBKey, YCSBPayload>& table;
    u64 tuples;
    std::vector<lsched::Request> pool;
    leanstore::utils::RingBuffer<lsched::Request*> free_list;
    std::atomic<bool> done{false};
    std::atomic<u64> t_measure_begin{0};
    std::atomic<u64> t_measure_end{~0ull};
    u64 offered = 0;  // requests created in the measured window
    u64 dropped = 0;  // no free request or queue full (open loop overload)

    Generator(lsched::Scheduler& s, BTreeInterface<YCSBKey, YCSBPayload>& t, u64 tuples, int n)
        : sched(s), table(t), tuples(tuples), pool(n), free_list(n)
    {
        for (auto& r : pool) {
            free_list.try_push(&r);
        }
    }

    void run()
    {
        utils::ScrambledZipfGenerator zipf(0, tuples, cfg.zipf);
        std::mt19937_64 rng(mean::readTSC());
        std::uniform_int_distribution<int> pct(0, 99);
        std::exponential_distribution<double> gap(cfg.rate > 0 ? cfg.rate : 1.0);

        const u64 start = mean::readTSC();
        const u64 begin = start + u64(cfg.warmup_s * 1e9 / ns_per_tick);
        const u64 end = begin + u64(cfg.run_s * 1e9 / ns_per_tick);
        t_measure_begin = begin;
        t_measure_end = end;
        u64 next = start;

        for (;;) {
            u64 now = mean::readTSC();
            if (now >= end) {
                break;
            }
            if (cfg.rate > 0) {
                if (now < next) {
                    continue;  // spin until the next arrival
                }
                now = next;  // arrival time is the scheduled one, not when we noticed
                next += u64(gap(rng) * 1e9 / ns_per_tick);
            }
            lsched::Request* r = nullptr;
            if (!free_list.try_pop(r)) {
                if (cfg.rate > 0 && now >= begin) {
                    dropped++;  // open loop: the system can't keep up
                }
                continue;  // closed loop: wait for a request to come back
            }
            r->type = pct(rng) < cfg.read ? lsched::ReqType::Lookup : lsched::ReqType::Update;
            r->key = zipf.rand();
            r->t_arrive = cfg.rate > 0 ? now : mean::readTSC();
            if (r->t_arrive >= begin) {
                offered++;
            }
            if (!sched.submit(r)) {
                free_list.try_push(r);
                if (r->t_arrive >= begin) {
                    dropped++;
                }
            }
        }
        done = true;
    }
};

// ---------------------------------------------------------------------------
// One slot: a LeanStore task on one executor (core) that runs one request at
// a time, as long as the scheduler gives it work.
// ---------------------------------------------------------------------------
void slotLoop(lsched::Scheduler& sched, Generator& gen, std::vector<lsched::CoreStats>& stats)
{
    const int core = mean::exec::getId();
    auto& st = stats[core];
    auto& table = gen.table;
    for (;;) {
        lsched::Request* r = sched.next(core);
        if (!r) {
            if (gen.done && sched.drained()) {
                break;
            }
            mean::task::yield();  // let the other slots of this core run
            continue;
        }
        r->core = core;
        r->t_start = mean::readTSC();
        if (r->type == lsched::ReqType::Lookup) {
            YCSBPayload result;
            table.lookup(r->key, result);
        } else {
            YCSBPayload payload;
            utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
            table.update(r->key, payload);
        }
        r->t_done = mean::readTSC();
        sched.finished(core);
        if (r->t_arrive >= gen.t_measure_begin && r->t_done <= gen.t_measure_end) {
            st.record(*r, ns_per_tick);
            st.completed++;
            st.busy_tsc += r->t_done - r->t_start;
        }
        gen.free_list.try_push(r);
        mean::task::yield();
    }
}

void report(lsched::Scheduler& sched, Generator& gen, std::vector<lsched::CoreStats>& stats)
{
    const double secs = (gen.t_measure_end - gen.t_measure_begin) * ns_per_tick / 1e9;
    lsched::CoreStats all;
    for (auto& s : stats) {
        for (size_t t = 0; t < s.lat.size(); t++) {
            for (int m = 0; m < lsched::kMetrics; m++) {
                all.lat[t][m].merge(s.lat[t][m]);
            }
        }
        all.completed += s.completed;
    }
    printf("lsched,config,workers=%d,slots=%d,queue=%s,place=%s,rate=%.0f,inflight=%d,run=%.1f,read=%d,"
           "zipf=%.2f,data_gib=%.2f,dram_gib=%.2f,io=%s,tsc_ghz=%.3f\n",
           cfg.workers, cfg.slots, sched.queue().name(), sched.placement().name(), cfg.rate, cfg.inflight,
           cfg.run_s, cfg.read, cfg.zipf, cfg.data_gib, cfg.dram_gib, cfg.io.c_str(), 1.0 / ns_per_tick);
    printf("lsched,summary,seconds,offered,completed,dropped,offered_rps,completed_rps\n");
    printf("lsched,summary,%.3f,%lu,%lu,%lu,%.0f,%.0f\n", secs, gen.offered, all.completed, gen.dropped,
           gen.offered / secs, all.completed / secs);
    printf("lsched,lat,type,metric,count,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n");
    for (size_t t = 0; t < all.lat.size(); t++) {
        for (int m = 0; m < lsched::kMetrics; m++) {
            auto& h = all.lat[t][m];
            if (h.count() == 0) {
                continue;
            }
            printf("lsched,lat,%s,%s,%lu,%.0f,%lu,%lu,%lu,%lu,%lu\n", lsched::reqTypeName(lsched::ReqType(t)),
                   lsched::metricName(m), h.count(), h.mean(), h.quantile(0.5), h.quantile(0.9), h.quantile(0.99),
                   h.quantile(0.999), h.max());
        }
    }
    printf("lsched,core,id,vcpu,completed,placed,pulled,busy_pct\n");
    auto& cores = sched.cores();
    for (size_t c = 0; c < stats.size(); c++) {
        printf("lsched,core,%zu,%zu,%lu,%lu,%lu,%.1f\n", c, c + cfg.first_core, stats[c].completed,
               cores[c].placed.load(), cores[c].pulled.load(),
               100.0 * stats[c].busy_tsc * ns_per_tick / 1e9 / secs);
    }
}

void runBenchmark()
{
    LeanStore db;
    std::unique_ptr<BTreeInterface<YCSBKey, YCSBPayload>> adapter;
    mean::task::scheduleTaskSync([&]() {
        auto& vs_btree = db.registerBTreeLL("ycsb", "y");
        adapter.reset(new BTreeVSAdapter<YCSBKey, YCSBPayload>(vs_btree));
    });
    auto& table = *adapter;
    const u64 tuples = cfg.data_gib * 1024.0 * 1024.0 * 1024.0 / 2.0 / (sizeof(YCSBKey) + sizeof(YCSBPayload));

    // --- load -----------------------------------------------------------------
    {
        printf("lsched: loading %lu tuples on %d workers\n", tuples, cfg.workers);
        const u64 t0 = mean::readTSC();
        std::atomic<u64> next_block{0};
        const u64 block = 1 << 16;
        auto insert = [&]() {
            for (;;) {
                const u64 b = next_block.fetch_add(block, std::memory_order_relaxed);
                if (b >= tuples) {
                    break;
                }
                const u64 e = std::min(b + block, tuples);
                for (u64 k = b; k < e; k++) {
                    YCSBPayload payload;
                    utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
                    table.insert(k, payload);
                }
            }
        };
        BlockedRange bb(0, (u64)1);
        mean::task::parallelFor(bb, insert, cfg.load_tasks, -1, false);
        const u64 pages = db.getBufferManager().consumedPages();
        printf("lsched: loaded in %.2f s, %lu pages (%lu MiB)\n", (mean::readTSC() - t0) * ns_per_tick / 1e9, pages,
               pages * PAGE_SIZE / 1024 / 1024);
    }

    // --- scheduled run --------------------------------------------------------
    lsched::Scheduler sched(cfg.workers, cfg.slots, lsched::makeQueue(cfg.queue, 1 << 16),
                            lsched::makePlacement(cfg.place));
    std::vector<lsched::CoreStats> stats(cfg.workers);
    Generator gen(sched, table, tuples, cfg.inflight);

    printf("lsched: run %.1f s (+%.1f s warmup), queue=%s place=%s rate=%.0f\n", cfg.run_s, cfg.warmup_s,
           sched.queue().name(), sched.placement().name(), cfg.rate);
    auto gen_thread = sched::thread::make_unique([&gen] { gen.run(); },
                                                 sched::thread::attr().name("lsched-gen").pin(sched::cpus[cfg.gen_cpu]));
    gen_thread->start();

    BlockedRange bb(0, (u64)1);
    mean::task::parallelFor(bb, [&]() { slotLoop(sched, gen, stats); }, cfg.slots, -1, false);
    gen_thread->join();

    report(sched, gen, stats);
    printf("lsched: done\n");
    mean::env::shutdown();
}
}  // namespace

// The OSv kernel calls osv_app_main() at boot.
extern "C" void osv_app_main()
{
    try {
        cfg = parseConfig();
        // fail early on a bad policy name, before the load phase
        lsched::makeQueue(cfg.queue, 2);
        lsched::makePlacement(cfg.place);
    } catch (std::exception& e) {
        printf("lsched: %s\n", e.what());
        return;
    }
    applyConfig(cfg);
    ns_per_tick = nsPerTick();

    mean::IoOptions ioOptions(cfg.io, "");
    ioOptions.write_back_buffer_size = PAGE_SIZE;
    ioOptions.engine = cfg.io;
    ioOptions.iodepth = 2;
    ioOptions.channelCount = cfg.workers;

    mean::env::init(cfg.workers, 0 /*exclusive threads*/, ioOptions, cfg.first_core);
    mean::env::start(runBenchmark);
    mean::env::join();
}
