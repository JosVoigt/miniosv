/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.hh>
#include <osv/irqlock.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/fault.hh>
#include <osv/sched.hh>

#include "arch-cpu.hh"
#include "exceptions.hh"

void page_fault(exception_frame *ef)
{
    sched::fpu_lock fpu;
    SCOPE_LOCK(fpu);
    sched::exception_guard g;
    auto addr = processor::read_cr2();
    if (fixup_fault(ef)) {
        return;
    }
    auto pc = reinterpret_cast<void*>(ef->rip);
    if (!pc) {
        // A call through a null pointer leaves its return address on top of
        // the stack: that names the caller, which the backtrace (stuck in the
        // fault entry) does not.
        auto *sp = reinterpret_cast<void **>(ef->rsp);
        if (sp) {
            for (int i = 0; i < 16; i++) {
                debug_early_u64("  stack word ", reinterpret_cast<u64>(sp[i]));
            }
        }
        abort("trying to execute null pointer (thread '%s', caller %p, rsp %p, rbp %p)",
              sched::thread::current()->name().c_str(), sp ? sp[0] : nullptr, sp,
              reinterpret_cast<void *>(ef->rbp));
    }
    // The following code may sleep. So let's verify the fault did not happen
    // when preemption was disabled, or interrupts were disabled.
    assert(sched::preemptable());
    assert(ef->rflags & processor::rflags_if);

    // And since we may sleep, make sure interrupts are enabled.
    DROP_LOCK(irq_lock) { // irq_lock is acquired by HW
        mem::vm_fault(addr, ef);
    }
}

namespace mem {
namespace mapping {

enum {
    page_fault_prot  = 1ul << 0,
    page_fault_write = 1ul << 1,
    page_fault_user  = 1ul << 2,
    page_fault_rsvd  = 1ul << 3,
    page_fault_insn  = 1ul << 4,
};

bool is_page_fault_insn(unsigned int error_code) {
    return error_code & page_fault_insn;
}

bool is_page_fault_write(unsigned int error_code) {
    return error_code & page_fault_write;
}

bool is_page_fault_rsvd(unsigned int error_code) {
    return error_code & page_fault_rsvd;
}

bool is_page_fault_prot_write(unsigned int error_code) {
    return (error_code & (page_fault_write | page_fault_prot)) == (page_fault_write | page_fault_prot);
}

bool fast_sigsegv_check(uintptr_t addr, exception_frame* ef)
{
    // A range with no permissions keeps a reserved bit set, so the fault it
    // raises says what it is without any lookup at all.
    if (is_page_fault_rsvd(ef->get_error())) {
        return true;
    }

    // A write to a page that is present but read-only is a permission error:
    // nothing here maps a page read-only and then wants a write to fill it in.
    if (is_page_fault_prot_write(ef->get_error())) {
        auto e = mem::mapping::find(addr);
        return e && !(e.perm() & mem::perm_write);
    }

    return false;
}

}
}
