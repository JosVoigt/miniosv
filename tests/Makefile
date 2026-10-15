# The test application: a small driver (osv_tests.cc) plus the OS/libc++
# correctness and stress suites. Linked into the kernel instead of the normal
# application with:
#
#     make app=test
#
# This file is included by the top-level Makefile, which compiles these sources
# with the kernel's own flags and links them into the kernel image.
#
#   - osv_tests.cc : the osv_app_main() driver that runs the suites below
#   - os-features  : OS facility conformance checks
#   - os-libc      : C libc surface conformance checks (string/stdlib/math/...)
#   - os-iostream  : C++ iostreams / localization checks (std::cout etc.)
#   - os-memmove   : memmove() overlap-correctness regression test
#   - os-string    : memcmp() correctness + throughput
#   - os-memory    : the memory clients: early, heap, page cache, plus malloc/mmap
#   - os-memory-primitives : vspace, frames and mapping, one header per function
#   - os-stress    : concurrency / allocator / TLS stress tests
#   - os-perf      : osv/perf.hh on the hardware counters (skipped without a PMU)
#   - os-miniext   : miniext filesystem checks, only when conf_fs_miniext=1
#                    (needs --emulated-nvme at run time; skips without)

app-objects =  tests/osv_tests.o
app-objects += tests/os-features.o
app-objects += tests/os-libc.o
app-objects += tests/os-iostream.o
app-objects += tests/os-memmove.o
app-objects += tests/os-string.o
app-objects += tests/os-memory.o
app-objects += tests/os-memory-primitives.o
app-objects += tests/os-stress.o
app-objects += tests/os-perf.o
ifeq ($(conf_fs_miniext),1)
app-objects += tests/os-miniext.o
endif
