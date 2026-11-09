/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef NVME_DRIVER_H
#define NVME_DRIVER_H

#include "drivers/driver.hh"
#include "drivers/nvme-queue.hh"
#include "drivers/pci-device.hh"
#include <map>
#include <memory>
#include <osv/aligned_new.hh>
#include <osv/interrupt.hh>
#include <osv/mempool.hh>
#include <osv/msi.hh>
#include <osv/nvme-structs.h>
#include <vector>

// Volatile Write Cache
#define NVME_VWC_ENABLED 1

#define NVME_ADMIN_QUEUE_SIZE 8
#define NVME_IO_QUEUE_SIZE 32
#define NVME_NAMESPACE_DEFAULT_NS 1

#define NVME_CTRL_CONFIG_IO_CQ_ENTRY_SIZE_16_BYTES 4
#define NVME_CTRL_CONFIG_IO_SQ_ENTRY_SIZE_64_BYTES 6
#define NVME_CTRL_CONFIG_PAGE_SIZE_4K 0

// APST leaves out power states whose exit latency is above this, as Linux'
// default_ps_max_latency_us does.
#define NVME_APST_DEFAULT_MAX_LATENCY_US 100000

namespace nvme {

enum NVME_IO_QUEUE_PRIORITY {
  NVME_IO_QUEUE_PRIORITY_URGENT = 0,
  NVME_IO_QUEUE_PRIORITY_HIGH = 1,
  NVME_IO_QUEUE_PRIORITY_MEDIUM = 2,
  NVME_IO_QUEUE_PRIORITY_LOW = 3,
};

enum CMD_IDENTIFY_CNS {
  CMD_IDENTIFY_NAMESPACE = 0,
  CMD_IDENTIFY_CONTROLLER = 1,
};

enum NVME_CONTROLLER_EN {
  CTRL_EN_DISABLE = 0,
  CTRL_EN_ENABLE = 1,
};

// Driver for a single NVMe controller (one PCI function). It owns the admin
// queue used to bring the controller up and configure it, and a set of I/O
// queues created on demand. Completions are delivered through MSI-X: every
// queue's interrupt vector simply calls queue_pair::process_completions().
class nvme_driver : public hw_driver {
public:
  explicit nvme_driver(pci::device &dev);
  virtual ~nvme_driver() {}

  // hw_driver interface
  virtual std::string get_name() const { return "nvme"; }
  virtual void dump_config();
  static hw_driver *probe(hw_device *dev);
  static std::vector<nvme_driver *> nvme_drives;
  
  // --- I/O queue management (public API used by the app/io backend) ---

  // Create an I/O queue pair of the given depth and return an opaque handle
  // (really an io_queue_pair*). target_interrupt_cpu pins the queue's MSI-X
  // completion interrupt; nullptr uses the current CPU.
  // interrupts=false: no completion interrupt at all (CQ created with IEN=0);
  // the caller polls the queue with process_completions().
  void *create_io_queue(int qsize, sched::cpu *target_interrupt_cpu = nullptr,
                        bool interrupts = true);

  // Largest single read or write, in bytes; submit_request() refuses larger.
  u32 max_transfer_bytes() const { return _max_transfer_bytes; }
  void remove_io_user_queue(io_queue_pair *queue);

  bool reset_and_destroy_controller();
  bool shutdown_controller();

  // --- health and power management ---
  // These issue admin commands and block until they complete. They return 0 or
  // a positive errno: EINVAL for an argument the controller can't take,
  // ENOTSUP for a feature it doesn't have, EIO when the command fails, ENOMEM
  // when there is no memory for the command's data.

  const nvme_identify_ctlr_t &identify() const { return *_identify_controller; }

  // SMART / Health Information log page for the whole controller.
  int get_smart_log(nvme_log_page_health_t &log);

  // Interrupt Coalescing: an interrupt after threshold + 1 completions or
  // time x 100 us, whichever comes first; 0 and 0 interrupt on every one.
  int get_interrupt_coalescing(u8 &threshold, u8 &time);
  // Whether the volatile write cache is on; ENOTSUP without one.
  int get_write_cache(bool &enabled);

  // Power states are numbered 0 .. power_state_count() - 1, 0 being the one
  // with the highest power and performance.
  unsigned power_state_count() const { return _identify_controller->npss + 1u; }
  const nvme_power_state_t &power_state(unsigned ps) const;

  // The power state set with set_power_state() or by APST. A non-operational
  // state is left for the last operational one as soon as I/O arrives.
  // set_power_state() fails with ENOTSUP where the controller doesn't let
  // the host change the power state.
  int get_power_state(u8 &ps);
  int set_power_state(u8 ps);

  // Autonomous Power State Transitions: the controller drops into deeper
  // states by itself after being idle for a while.
  bool apst_supported() const { return _identify_controller->apsta & 0x1; }
  int get_apst(bool &enabled, nvme_apst_table_t *table = nullptr);
  // Enabling builds the table the way Linux does: from every state, go to the
  // deepest non-operational state whose exit latency is at most
  // max_exit_latency_us, after 50 times its entry + exit latency of idleness.
  // Fails with EINVAL if no state qualifies. Disabling clears the table.
  int set_apst(bool enable,
               u32 max_exit_latency_us = NVME_APST_DEFAULT_MAX_LATENCY_US);
  // Enable or disable APST with a table of the caller's own.
  int set_apst_table(bool enable, const nvme_apst_table_t &table);

  // --- multi-controller registry ---
  static nvme_driver *get_nvme_device(int id);
  const int get_id() { return _id; }

  // Namespace geometry, keyed by nsid (normally a single entry keyed by 1).
  // Shared (by reference) with every queue_pair so they can size transfers.
  std::map<u32, nvme_ns_t *> _ns_data;

private:
  // --- controller bring-up / configuration ---
  bool parse_pci_config();
  void init_controller_config();
  int enable_disable_controller(bool enable);
  int wait_for_controller_ready_change(int ready);
  int wait_for_controller_shutdown_done();
  int get_worst_cast_time();

  // --- admin queue + admin commands ---
  void create_admin_queue();
  int identify_controller();
  int identify_namespace(u32 ns);
  int set_number_of_queues(u16 num, u16 *ret);
  int set_interrupt_coalescing(u8 threshold, u8 time);
  void enable_write_cache();
  // Get Features with cdw11 = 0; the feature's value comes back in *value
  // and, for features with a data structure, in data (one page, physical).
  int get_feature(u8 fid, u32 *value, void *data = nullptr);

  // --- interrupts (MSI-X) ---
  void enable_msix();
  // Wire MSI-X vector `iv` so its ISR drains `qp` via process_completions().
  // Used for both the admin queue (iv 0) and every I/O queue.
  bool msix_register_completion_interrupt(unsigned iv, queue_pair *qp,
                                          sched::cpu *affinity_cpu);

  // --- registry / instance bookkeeping ---
  static int _instance;
  int _id;

  // --- queues ---
  std::unique_ptr<admin_queue_pair, aligned_new_deleter<admin_queue_pair>>
      _admin_queue;
  std::vector<
      std::unique_ptr<io_queue_pair, aligned_new_deleter<io_queue_pair>>>
      _io_queues;
  size_t _queue_id_counter = 0;

  // --- controller registers / PCI ---
  pci::device &_dev;
  pci::bar *_bar0 = nullptr;
  nvme_controller_reg_t *_control_reg = nullptr;
  u32 _doorbell_stride;
  u32 _max_transfer_bytes = 0;
  u32 _qsize;

  std::unique_ptr<nvme_identify_ctlr_t> _identify_controller;

  // --- interrupt plumbing ---
  interrupt_manager _msi;
  std::vector<std::unique_ptr<msix_vector>> _msix_vectors;
};

// Units of the health and power data.
inline int kelvin_to_celsius(u16 kelvin) { return int(kelvin) - 273; }

inline double max_power_watts(const nvme_power_state_t &ps) {
  return ps.mp * (ps.mxps ? 0.0001 : 0.01);
}

// NaN where the controller doesn't report it (scale 0).
inline double scaled_power_watts(u16 value, u8 scale) {
  switch (scale) {
  case 1: return value * 0.0001;
  case 2: return value * 0.01;
  default: return __builtin_nan("");
  }
}

} // namespace nvme
#endif
