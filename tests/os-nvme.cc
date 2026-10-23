/*
 * NVMe health and power management.
 *
 * On every NVMe controller the driver found (under QEMU the boot disk is one):
 *
 *   - the SMART / health log reads and holds a plausible temperature
 *   - reads work, a transfer above the controller's limit is refused, and on
 *     a scratch controller writes of different sizes and buffer alignments
 *     (one block, across a page boundary, needing a PRP list, the largest
 *     transfer) read back unchanged
 *   - the power state table makes sense, every power state can be set and
 *     reads back, an out-of-range one is refused, and the original state is
 *     restored afterwards (where the controller lets the host change it at
 *     all; QEMU's doesn't)
 *   - APST: enabling builds a table that only goes to non-operational states
 *     and reads back as enabled, disabling reads back as disabled, and the
 *     original setting is restored. Without APST the calls report ENOTSUP.
 *
 * QEMU's emulated NVMe has a single power state and no APST, so most of the
 * power checks only do something on real hardware:
 *
 *     make app=tests && scripts/run.py --pass-pci <addr>
 *
 * Writes only go to a controller that is scratch space: the --emulated-nvme
 * drive (serial "deadbeef", see scripts/run.py), or the one whose serial is
 * given with `make app=tests nvme_test_write_serial=<serial>`. They overwrite
 * a few MiB from 64 MiB on.
 */

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <osv/sched.hh>

#include "drivers/nvme.hh"

#ifndef NVME_TEST_WRITE_SERIAL
#define NVME_TEST_WRITE_SERIAL ""
#endif

namespace {

int failures;
int checks;

void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  PASS  %s\n", what);
    }
}

// The 128-bit SMART counters; nothing reaches the high half in practice.
unsigned long long lo(const u64 (&counter)[2])
{
    return counter[1] ? ~0ull : counter[0];
}

void test_smart(nvme::nvme_driver &d)
{
    nvme_log_page_health_t log;
    memset(&log, 0xA5, sizeof(log));
    int err = d.get_smart_log(log);
    check(err == 0, "SMART log reads");
    if (err) {
        return;
    }

    int temp = nvme::kelvin_to_celsius(log.temp);
    printf("        critical warning %#x, temperature %d C, spare %u%% "
           "(threshold %u%%), used %u%%\n",
           log.warn, temp, log.avspare, log.avsparethresh, log.used);
    printf("        read %llu / written %llu data units, %llu power cycles, "
           "%llu power-on hours, %llu unsafe shutdowns, %llu media errors\n",
           lo(log.dur), lo(log.duw), lo(log.pcycles), lo(log.phours),
           lo(log.unsafeshut), lo(log.merrors));
    for (unsigned i = 0; i < 8; i++) {
        if (log.tsen[i]) {
            printf("        sensor %u: %d C\n", i + 1,
                   nvme::kelvin_to_celsius(log.tsen[i]));
        }
    }

    check(log.temp != 0 && temp > -40 && temp < 125,
          "composite temperature is plausible");
    check(log.avspare <= 100 && log.avsparethresh <= 100,
          "spare percentages are percentages");
}

// Identify strings are space padded, not NUL terminated.
bool id_string_is(const char *field, size_t size, const char *want)
{
    size_t len = size;
    while (len > 0 && (field[len - 1] == ' ' || field[len - 1] == 0)) {
        len--;
    }
    return len > 0 && strlen(want) == len && memcmp(field, want, len) == 0;
}

bool may_write(nvme::nvme_driver &d)
{
    const auto &id = d.identify();
    return id_string_is(id.sn, sizeof(id.sn), "deadbeef") ||
           id_string_is(id.sn, sizeof(id.sn), NVME_TEST_WRITE_SERIAL);
}

struct io_wait {
    sched::thread *thread;
    std::atomic<bool> done{false};
    nvme_cq_entry_t cqe;
};

// Runs in the queue's interrupt.
void io_done(void *ctx, const nvme_cq_entry_t *cqe)
{
    auto *w = static_cast<io_wait *>(ctx);
    // Once done is set the waiter may return and take w with it.
    auto *thread = w->thread;
    w->cqe = *cqe;
    w->done.store(true, std::memory_order_release);
    thread->wake_with_irq_disabled();
}

// One command, waited for. Returns 0, submit_request()'s error, or -1 when the
// command completed with an error status.
int io_sync(nvme::io_queue_pair *q, nvme::NVME_COMMAND op, u64 addr,
            void *buf, u32 len)
{
    io_wait w;
    w.thread = sched::thread::current();
    int rc;
    while ((rc = q->submit_request(NVME_NAMESPACE_DEFAULT_NS, buf, addr, len,
                                   io_done, &w, 0, op)) == 0) {
        sched::thread::yield(); // queue full
    }
    if (rc != 1) {
        return rc;
    }
    sched::thread::wait_until(
        [&] { return w.done.load(std::memory_order_acquire); });
    return (w.cqe.sct || w.cqe.sc) ? -1 : 0;
}

void fill(char *buf, u32 len, u64 seed)
{
    for (u32 i = 0; i < len; i += sizeof(u64)) {
        u64 v = seed ^ (u64(i) * 0x9E3779B97F4A7C15ull);
        memcpy(buf + i, &v, sizeof(v));
    }
}

void test_settings(nvme::nvme_driver &d)
{
    // The driver never turns coalescing on, and turns the write cache on
    // where there is one.
    u8 threshold = 0xFF, time = 0xFF;
    check(d.get_interrupt_coalescing(threshold, time) == 0 && threshold == 0 &&
              time == 0,
          "interrupt coalescing reads as off");
    bool wc = false;
    int err = d.get_write_cache(wc);
    if (d.identify().vwc & 0x1) {
        check(err == 0 && wc, "write cache reads as on");
    } else {
        check(err == ENOTSUP, "no write cache: ENOTSUP");
    }
}

void test_read_write(nvme::nvme_driver &d)
{
    const auto *ns = d._ns_data[NVME_NAMESPACE_DEFAULT_NS];
    u32 bs = ns->blocksize;
    u32 max = d.max_transfer_bytes();
    printf("        %u-byte blocks, largest transfer %u bytes\n", bs, max);
    check(max >= 4096 && max <= (1u << 20) && max % bs == 0,
          "largest transfer is between 4 KiB and 1 MiB");

    // Stays for the rest of the run: remove_io_user_queue() deletes the CQ
    // before its SQ, which the spec doesn't allow.
    auto *q = static_cast<nvme::io_queue_pair *>(d.create_io_queue(8));
    // Room for the largest transfer at an offset into its first page.
    size_t buf_size = max + 4096;
    auto *wbuf = static_cast<char *>(aligned_alloc(4096, buf_size));
    auto *rbuf = static_cast<char *>(aligned_alloc(4096, buf_size));
    if (!q || !wbuf || !rbuf) {
        check(false, "I/O queue and buffers");
        free(wbuf);
        free(rbuf);
        return;
    }

    check(io_sync(q, nvme::READ, 0, rbuf, bs) == 0, "block 0 reads");
    check(q->submit_request(NVME_NAMESPACE_DEFAULT_NS, rbuf, 0, max + bs,
                            io_done, nullptr, 0, nvme::READ) == EINVAL,
          "transfer above the largest is refused");

    u64 base = (64u << 20) / bs;
    if (!may_write(d)) {
        printf("        writes skipped: not a scratch controller\n");
    } else if (ns->blockcount < base + (8u << 20) / bs) {
        printf("        writes skipped: namespace too small\n");
    } else {
        struct {
            u32 len, buf_off, block;
            const char *what;
        } cases[] = {
            {bs, 0, 1, "one block"},
            {4096, 0, 8, "4 KiB"},
            {4096, 512, 17, "4 KiB across a page boundary"},
            {8192, 0, 40, "8 KiB"},
            {128 << 10, 0, 101, "128 KiB (PRP list)"},
            {128 << 10, 512, 400, "128 KiB across pages (PRP list)"},
            {max, 0, 1001, "largest transfer"},
        };
        char what[96];
        for (auto &c : cases) {
            if (c.len % bs || c.buf_off % 4) {
                continue;
            }
            u64 addr = (base + c.block) * bs;
            char *w = wbuf + c.buf_off, *r = rbuf + c.buf_off;
            fill(w, c.len, addr);
            memset(r, 0, c.len);
            int werr = io_sync(q, nvme::WRITE, addr, w, c.len);
            int rerr = werr ? 0 : io_sync(q, nvme::READ, addr, r, c.len);
            bool same = !werr && !rerr && memcmp(w, r, c.len) == 0;
            snprintf(what, sizeof(what), "%s writes and reads back%s", c.what,
                     werr ? " (write failed)"
                          : rerr ? " (read failed)" : same ? "" : " (differs)");
            check(same, what);
        }
    }

    free(wbuf);
    free(rbuf);
}

void print_power_states(nvme::nvme_driver &d)
{
    printf("        ps  max W     enlat us  exlat us  idle W   active W\n");
    for (unsigned ps = 0; ps < d.power_state_count(); ps++) {
        const auto &desc = d.power_state(ps);
        printf("        %2u  %-8.4f  %-8u  %-8u  %-7.4f  %-8.4f  %s\n", ps,
               nvme::max_power_watts(desc), (unsigned)desc.enlat,
               (unsigned)desc.exlat,
               nvme::scaled_power_watts(desc.idlp, desc.ips),
               nvme::scaled_power_watts(desc.actp, desc.aps),
               desc.nops ? "non-operational" : "operational");
    }
}

void test_power_states(nvme::nvme_driver &d)
{
    unsigned count = d.power_state_count();
    print_power_states(d);

    check(count >= 1 && count <= 32, "1 to 32 power states");
    check(!d.power_state(0).nops, "power state 0 is operational");

    u8 orig = 0xFF;
    check(d.get_power_state(orig) == 0 && orig < count,
          "current power state reads and is in range");
    if (orig >= count) {
        return;
    }

    // Setting the state it's already in shows whether the host may change it.
    int err = d.set_power_state(orig);
    if (err == ENOTSUP) {
        printf("        power state not changeable by the host\n");
        check(d.set_power_state(count) == EINVAL,
              "power state past the last one is refused");
        return;
    }
    check(err == 0, "current power state can be set again");

    // APST would move the controller on its own and spoil the read-backs.
    bool apst_was = false;
    nvme_apst_table_t apst_table;
    bool have_apst = d.apst_supported() &&
                     d.get_apst(apst_was, &apst_table) == 0;
    if (have_apst && apst_was) {
        d.set_apst(false);
    }

    char what[96];
    for (unsigned ps = 0; ps < count; ps++) {
        u8 now = 0xFF;
        int set_err = d.set_power_state(ps);
        int get_err = set_err ? 0 : d.get_power_state(now);
        bool ok = !set_err && !get_err && now == ps;
        if (ok) {
            snprintf(what, sizeof(what), "power state %u sets and reads back",
                     ps);
        } else {
            snprintf(what, sizeof(what),
                     "power state %u sets and reads back (set %d, get %d, "
                     "read %u)", ps, set_err, get_err, now);
        }
        check(ok, what);
    }

    check(d.set_power_state(count) == EINVAL,
          "power state past the last one is refused");

    u8 now = 0xFF;
    check(d.set_power_state(orig) == 0 && d.get_power_state(now) == 0 &&
              now == orig,
          "original power state restored");

    if (have_apst && apst_was) {
        d.set_apst_table(true, apst_table);
    }
}

void test_apst(nvme::nvme_driver &d)
{
    bool enabled = false;
    if (!d.apst_supported()) {
        printf("        APST not supported\n");
        check(d.get_apst(enabled) == ENOTSUP && d.set_apst(true) == ENOTSUP,
              "APST calls report ENOTSUP");
        return;
    }

    bool orig = false;
    nvme_apst_table_t orig_table;
    check(d.get_apst(orig, &orig_table) == 0, "APST setting reads");

    bool any_nops = false;
    for (unsigned ps = 0; ps < d.power_state_count(); ps++) {
        any_nops |= d.power_state(ps).nops;
    }

    int err = d.set_apst(true);
    if (!any_nops) {
        check(err == EINVAL, "no non-operational state: APST refused");
        return;
    }
    check(err == 0, "APST enables");

    nvme_apst_table_t table;
    check(d.get_apst(enabled, &table) == 0 && enabled, "APST reads as enabled");

    bool targets_ok = true;
    bool any_entry = false;
    for (unsigned ps = 0; ps < d.power_state_count(); ps++) {
        u64 e = table.entry[ps];
        if (!e) {
            continue;
        }
        any_entry = true;
        unsigned to = nvme_apst_itps(e);
        printf("        idle in %u for %u ms -> %u\n", ps, nvme_apst_itpt(e),
               to);
        targets_ok &= to > ps && to < d.power_state_count() &&
                      d.power_state(to).nops;
    }
    check(any_entry, "APST table has transitions");
    check(targets_ok, "APST only goes to deeper non-operational states");

    check(d.set_apst(false) == 0 && d.get_apst(enabled) == 0 && !enabled,
          "APST disables");

    check(d.set_apst_table(orig, orig_table) == 0 &&
              d.get_apst(enabled) == 0 && enabled == orig,
          "original APST setting restored");
}

} // namespace

int os_nvme_main()
{
    printf("nvme: health and power management\n");

    auto &drives = nvme::nvme_driver::nvme_drives;
    if (drives.empty()) {
        printf("  SKIP  no NVMe controller\n");
        return 0;
    }

    for (auto *d : drives) {
        const auto &id = d->identify();
        printf("\n nvme%d: %.40s (fw %.8s), %u power states, APST %s\n",
               d->get_id(), id.mn, id.fr, d->power_state_count(),
               d->apst_supported() ? "supported" : "not supported");
        test_smart(*d);
        test_settings(*d);
        test_read_write(*d);
        test_power_states(*d);
        test_apst(*d);
    }

    printf("\nnvme: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
