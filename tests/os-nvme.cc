/*
 * NVMe health.
 *
 * On every NVMe controller the driver found (under QEMU the boot disk is one):
 *
 *   - the SMART / health log reads and holds a plausible temperature
 *
 * On real hardware, pass the controller through:
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

} // namespace

int os_nvme_main()
{
    printf("nvme: health\n");

    auto &drives = nvme::nvme_driver::nvme_drives;
    if (drives.empty()) {
        printf("  SKIP  no NVMe controller\n");
        return 0;
    }

    for (auto *d : drives) {
        const auto &id = d->identify();
        printf("\n nvme%d: %.40s (fw %.8s)\n", d->get_id(), id.mn, id.fr);
        test_smart(*d);
    }

    printf("\nnvme: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
