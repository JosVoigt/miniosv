/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/mman.h>
#include <osv/align.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <memory>
#include <new>
#include <osv/debug.hh>
#include "osv/trace.hh"
#include <osv/stubbing.hh>
#include "libc/libc.hh"
#include <safe-ptr.hh>
#include <atomic>
#include <cstring>
#include <osv/kernel_config.h>

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0x4000000
#endif

TRACEPOINT(trace_memory_mmap, "addr=%p, length=%d, prot=%d, flags=%d, fd=%d, offset=%d", void *, size_t, int, int, int, off_t);
TRACEPOINT(trace_memory_mmap_err, "%d", int);
TRACEPOINT(trace_memory_mmap_ret, "%p", void *);
TRACEPOINT(trace_memory_munmap, "addr=%p, length=%d", void *, size_t);
TRACEPOINT(trace_memory_munmap_err, "%d", int);
TRACEPOINT(trace_memory_munmap_ret, "");

unsigned libc_prot_to_perm(int prot)
{
    unsigned perm = 0;
    if (prot & PROT_READ) {
        perm |= mem::perm_read;
    }
    if (prot & PROT_WRITE) {
        perm |= mem::perm_write;
    }
    if (prot & PROT_EXEC) {
        perm |= mem::perm_exec;
    }
    return perm;
}

static bool page_aligned(const void *p)
{
    return !(reinterpret_cast<uintptr_t>(p) & (mem::mapping::page_size - 1));
}

// Anonymous memory: a reservation of its own, backed on first touch.
// jemalloc reserves address space geometrically and gives ranges back with
// madvise, both of which only work if a reservation costs nothing until used.
static size_t anon_leaf(mem::range s)
{
    constexpr size_t huge = mem::mapping::huge_page_size;
    return (s.start % huge == 0 && s.size() % huge == 0) ? huge : mem::mapping::page_size;
}

static bool mapped(uintptr_t addr)
{
    auto e = mem::mapping::find(addr);
    return e && e.present();
}

static bool anon_fault(mem::vspace::region &r, uintptr_t addr, unsigned)
{
    size_t leaf = anon_leaf(r.span);
    uintptr_t base = align_down(addr, leaf);
    // Losing the race to another cpu leaves the leaf mapped, which is as good.
    return mem::mapping::populate({base, base + leaf}, r.perm, leaf) || mapped(addr);
}

static const mem::vspace::region_ops anon_ops = { .fault = anon_fault };

static mem::vspace::region *anon_at(const void *addr)
{
    auto *r = mem::vspace::lookup(reinterpret_cast<uintptr_t>(addr));
    return r && r->ops == &anon_ops ? r : nullptr;
}

static void *anon_map(size_t length, unsigned perm, bool eager)
{
    // Huge leaves once there is enough to fill one.
    size_t leaf = length >= mem::mapping::huge_page_size ?
                  mem::mapping::huge_page_size : mem::mapping::page_size;
    auto *r = new (std::nothrow) mem::vspace::region();
    if (!r) {
        return nullptr;
    }
    r->perm = perm;
    r->ops = &anon_ops;
    if (mem::vspace::reserve(*r, align_up(length, leaf), leaf) !=
        mem::vspace::resa_result::success) {
        delete r;
        return nullptr;
    }
    if (eager && !mem::mapping::populate(r->span, perm, leaf)) {
        mem::mapping::depopulate(r->span);
        mem::vspace::release(*r);
        delete r;
        return nullptr;
    }
    return reinterpret_cast<void *>(r->span.start);
}

// depopulate invalidates before the frames go back, so the addresses this is
// giving up cannot be reached through a stale translation.
static void anon_unmap(mem::vspace::region *r)
{
    mem::mapping::depopulate(r->span);
    mem::vspace::release(*r);
    delete r;
}

OSV_LIBC_API
int mprotect(void *addr, size_t len, int prot)
{
    if (!page_aligned(addr)) {
        return libc_error(EINVAL);
    }

    // Only a mapping this made can be reprotected: anything else shares its
    // pages with the allocation next to it.
    len = align_up(len, mem::mapping::page_size);
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    auto *r = anon_at(addr);
    if (!r || !r->span.contains({start, start + len})) {
        return libc_error(ENOMEM);
    }
    mem::mapping::protect({start, start + len}, libc_prot_to_perm(prot));
    return 0;
}

int mmap_validate(void *addr, size_t length, int flags, off_t offset)
{
    int type = flags & (MAP_SHARED|MAP_PRIVATE);
    // Either MAP_SHARED or MAP_PRIVATE must be set, but not both.
    if (!type || type == (MAP_SHARED|MAP_PRIVATE)) {
        return EINVAL;
    }
    if ((flags & MAP_FIXED && !page_aligned(addr)) ||
        !page_aligned(reinterpret_cast<void *>(offset)) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
    trace_memory_mmap(addr, length, prot, flags, fd, offset);

    int err = mmap_validate(addr, length, flags, offset);
    if (err) {
        errno = err;
        trace_memory_mmap_err(err);
        return MAP_FAILED;
    }

    void *ret;

    auto mmap_perm = libc_prot_to_perm(prot);

    // There is no filesystem, so only anonymous mappings are supported;
    // file-backed mmap is not available.
    if (!(flags & MAP_ANONYMOUS)) {
        errno = ENODEV;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    // MAP_FIXED has no answer here, since the address space manager picks
    // addresses.
    if (flags & MAP_FIXED) {
        errno = ENOTSUP;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    // Stacks stay eager: the kernel has no lazy-stack support.
    ret = anon_map(length, mmap_perm, flags & (MAP_POPULATE | MAP_STACK));
    if (!ret) {
        errno = ENOMEM;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    trace_memory_mmap_ret(ret);
    return ret;
}

int munmap_validate(void *addr, size_t length)
{
    if (!page_aligned(addr) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
int munmap(void *addr, size_t length)
{
    trace_memory_munmap(addr, length);
    int error = munmap_validate(addr, length);
    if (error) {
        errno = error;
        trace_memory_munmap_err(error);
        return -1;
    }
    int ret = 0;
    // A mapping is given back whole, at the address mmap() returned.
    auto *r = anon_at(addr);
    if (r && r->span.start == reinterpret_cast<uintptr_t>(addr)) {
        anon_unmap(r);
    } else {
        errno = EINVAL;
        ret = -1;
        trace_memory_munmap_err(errno);
    }
    trace_memory_munmap_ret();
    return ret;
}

// Anonymous memory has no backing store, so this only reports whether the
// range is there at all.
OSV_LIBC_API
int msync(void *addr, size_t length, int flags)
{
    if (!anon_at(addr)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// DONTNEED and FREE drop whole leaves and zero the rest, so the range reads
// as zero afterwards either way, which is what jemalloc's purge assumes.
OSV_LIBC_API
int madvise(void *addr, size_t length, int advice)
{
    auto *r = anon_at(addr);
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (!r || !r->span.contains({start, start + length})) {
        errno = ENOMEM;
        return -1;
    }
    if (advice != MADV_DONTNEED && advice != MADV_FREE) {
        return 0;
    }
    size_t leaf = anon_leaf(r->span);
    uintptr_t end = start + length;
    uintptr_t lo = align_up(start, leaf), hi = align_down(end, leaf);
    auto zero = [](uintptr_t a, uintptr_t b) {
        if (a < b && mapped(a)) {
            memset(reinterpret_cast<void *>(a), 0, b - a);
        }
    };
    if (lo < hi) {
        mem::mapping::depopulate({lo, hi});
        zero(start, lo);
        zero(hi, end);
    } else {
        zero(start, end);
    }
    return 0;
}

// brk/sbrk are not supported: nothing asks for them, and a program break wants
// a lazily backed region, which anonymous memory here is not.
OSV_LIBC_API
int brk(void *)
{
    errno = ENOMEM;
    return -1;
}

OSV_LIBC_API
void *sbrk(intptr_t)
{
    errno = ENOMEM;
    return (void *)-1;
}

OSV_LIBC_API
int posix_madvise(void *addr, size_t len, int advice) {
    return anon_at(addr) ? 0 : ENOMEM;
}
