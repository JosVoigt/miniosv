/*
 * NVMe health and power management.
 *
 * On every NVMe controller the driver found (under QEMU the boot disk is one):
 *
 *   - the SMART / health log reads and holds a plausible temperature
 *   - the power state table makes sense, every power state can be set and
 *     reads back, an out-of-range one is refused, and the original state is
 *     restored afterwards (where the controller lets the host change it at
 *     all; QEMU's doesn't)
 *
 * QEMU's emulated NVMe has a single power state, so most of the power checks
 * only do something on real hardware:
 *
 *     make app=tests && scripts/run.py --pass-pci <addr>
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "drivers/nvme.hh"

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
        printf("\n nvme%d: %.40s (fw %.8s), %u power states\n",
               d->get_id(), id.mn, id.fr, d->power_state_count());
        test_smart(*d);
        test_power_states(*d);
    }

    printf("\nnvme: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
