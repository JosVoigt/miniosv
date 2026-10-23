# The test application: a small driver (osv_tests.cc) plus the OS/libc++
# correctness and stress suites. Linked into the kernel instead of the normal
# application with:
#
#     make app=tests
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
#   - os-stress    : concurrency / allocator / TLS stress tests
#   - os-nvme      : NVMe health and power management (conf_drivers_nvme)

app-objects = tests/osv_tests.o
app-objects += tests/os-features.o
app-objects += tests/os-libc.o
app-objects += tests/os-iostream.o
app-objects += tests/os-memmove.o
app-objects += tests/os-string.o
app-objects += tests/os-stress.o
ifeq ($(conf_drivers_nvme),1)
app-objects += tests/os-nvme.o
endif

# Serial of a controller os-nvme may write to besides QEMU's --emulated-nvme
# drive (e.g. a passed-through disk whose data may go). A stamp rebuilds
# os-nvme.o when it changes.
nvme_test_write_serial ?=
nvme_test_write_serial_dep = $(out)/nvme_test_write_serial.last
.PHONY: nvme_test_write_serial_phony
$(nvme_test_write_serial_dep): nvme_test_write_serial_phony
	$(call very-quiet, $(makedir))
	@if [ "$$(cat $@ 2>/dev/null)" != "$(nvme_test_write_serial)" ]; then \
		echo -n "$(nvme_test_write_serial)" > $@; \
	fi
$(out)/tests/os-nvme.o: $(nvme_test_write_serial_dep)
$(out)/tests/os-nvme.o: CXXFLAGS += -DNVME_TEST_WRITE_SERIAL='"$(nvme_test_write_serial)"'
