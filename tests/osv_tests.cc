/*
 * In-kernel test application.
 *
 * Built instead of the normal application with `make app=tests`. Like any
 * application it is statically linked into the kernel image and entered through
 * osv_app_main().
 *
 * With no boot arguments every suite runs. Otherwise the arguments name the
 * suites to run, in the order given:
 *
 *     scripts/run.py --args "memory"
 *     scripts/run.py --args "libc iostream"
 *     scripts/run.py --args "--list"
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <osv/bootargs.hh>
#include <osv/kernel_config.h>
#include <osv/power.hh>

int os_features_main();
int os_libc_main();
int os_stress_main();
int os_iostream_main();
int os_memmove_main();
int os_string_main();
int os_memory_main();
int os_memory_primitives_main();
int os_perf_main();
#if CONF_fs_miniext
int os_miniext_main();
#endif

namespace {

struct suite {
	const char *name;
	int (*run)();
	const char *what;
};

// The default order is also the order they run in when nothing is selected:
// conformance first, then the long-running ones.
const suite suites[] = {
	{"features", os_features_main, "OS facility conformance"},
	{"libc",     os_libc_main,     "C libc surface conformance"},
	{"iostream", os_iostream_main, "C++ iostreams and localization"},
	{"memmove",  os_memmove_main,  "memmove() overlap correctness"},
	{"string",   os_string_main,   "memcmp() correctness and throughput"},
	{"memory",   os_memory_main,   "the memory clients: early, heap, page cache"},
	{"memory-primitives", os_memory_primitives_main, "the memory primitives: vspace, frames, mapping"},
#if CONF_fs_miniext
	{"miniext",  os_miniext_main,  "miniext filesystem (needs --emulated-nvme)"},
#endif
	{"stress",   os_stress_main,   "concurrency and allocator stress"},
	{"perf",     os_perf_main,     "osv/perf.hh on the hardware counters"},
};

const suite *find_suite(const std::string &name)
{
	for (const auto &s : suites) {
		if (name == s.name) {
			return &s;
		}
	}
	return nullptr;
}

void list()
{
	printf("suites:\n");
	for (const auto &s : suites) {
		printf("  %-9s %s\n", s.name, s.what);
	}
	printf("\nrun a subset with: scripts/run.py --args \"<suite> [suite...]\"\n");
}

} // namespace

extern "C" void osv_app_main()
{
	printf("\n######## OSv test application ########\n\n");

	std::vector<std::string> words = osv::bootargs_split(osv::bootargs());

	for (const auto &w : words) {
		if (w == "--list" || w == "-l") {
			list();
			osv::poweroff();
		}
	}

	std::vector<const suite *> selected;
	for (const auto &w : words) {
		const suite *s = find_suite(w);
		if (!s) {
			printf("no suite named '%s'.\n\n", w.c_str());
			list();
			printf("\n######## OSv test application: FAILURE ########\n\n");
			osv::poweroff();
		}
		selected.push_back(s);
	}
	if (selected.empty()) {
		for (const auto &s : suites) {
			selected.push_back(&s);
		}
	}

	int rc = 0;
	for (size_t i = 0; i < selected.size(); i++) {
		if (i) {
			printf("\n");
		}
		rc |= selected[i]->run();
	}

	printf("\n######## OSv test application: %s ########\n\n",
	       rc ? "FAILURE" : "SUCCESS");

	osv::poweroff();
}
