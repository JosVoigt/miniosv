#pragma once
#include "Units.hpp"
#include <string>
// -------------------------------------------------------------------------------------
// Configuration (formerly gflags command-line flags).
//
// The defaults live in Config.cpp. They are plain globals, so the app may
// overwrite them from the boot arguments before it builds the LeanStore
// instance (see app/app.cc); nothing reads them before that.
//
// The historical `FLAGS_<name>` spelling is kept so existing call sites compile
// unchanged.
// -------------------------------------------------------------------------------------
extern std::string FLAGS_free_pages_list_path;
// -------------------------------------------------------------------------------------
extern double FLAGS_dram_gib;
extern double FLAGS_ssd_gib;
extern u32 FLAGS_cool_pct;
extern u32 FLAGS_free_pct;
extern u32 FLAGS_partition_bits;
extern u32 FLAGS_pp_threads;
// -------------------------------------------------------------------------------------
extern std::string FLAGS_csv_path;
extern bool FLAGS_csv_truncate;
extern std::string FLAGS_ssd_path;
extern u32 FLAGS_async_batch_size;
extern bool FLAGS_trunc;
extern u32 FLAGS_falloc;
// -------------------------------------------------------------------------------------
extern bool FLAGS_print_debug;
extern bool FLAGS_print_tx_console;
extern u32 FLAGS_print_debug_interval_s;
extern bool FLAGS_print_obj_stats;
extern bool FLAGS_profiling;
// -------------------------------------------------------------------------------------
extern u32 FLAGS_worker_threads;
extern u32 FLAGS_worker_tasks;
extern bool FLAGS_nopp;
extern bool FLAGS_pin_threads;
extern bool FLAGS_smt;
// -------------------------------------------------------------------------------------
extern bool FLAGS_root;
// -------------------------------------------------------------------------------------
extern u64 FLAGS_backoff_strategy;
// -------------------------------------------------------------------------------------
extern std::string FLAGS_zipf_path;
extern double FLAGS_zipf_factor;
extern double FLAGS_target_gib;
extern u64 FLAGS_run_for_seconds;
extern u64 FLAGS_warmup_for_seconds;
// -------------------------------------------------------------------------------------
extern bool FLAGS_contention_split;
extern u64 FLAGS_cm_update_on;
extern u64 FLAGS_cm_period;
extern u64 FLAGS_cm_slowpath_threshold;
// -------------------------------------------------------------------------------------
extern bool FLAGS_xmerge;
extern u64 FLAGS_xmerge_k;
extern double FLAGS_xmerge_target_pct;
// -------------------------------------------------------------------------------------
extern u64 FLAGS_backoff;
// -------------------------------------------------------------------------------------
extern u64 FLAGS_x;
extern u64 FLAGS_y;
extern double FLAGS_d;
// -------------------------------------------------------------------------------------
extern bool FLAGS_bulk_insert;
// -------------------------------------------------------------------------------------
extern s64 FLAGS_trace_dt_id;
extern s64 FLAGS_trace_trigger_probability;
extern std::string FLAGS_tag;
// -------------------------------------------------------------------------------------
extern bool FLAGS_out_of_place;
extern bool FLAGS_optimistic_parent_pointer;
// -------------------------------------------------------------------------------------
extern bool FLAGS_wal;
extern u64 FLAGS_wal_offset_gib;
extern bool FLAGS_wal_io_hack;
extern bool FLAGS_wal_fsync;
// -------------------------------------------------------------------------------------
extern bool FLAGS_si;
extern u64 FLAGS_si_refresh_rate;
extern bool FLAGS_vw;
extern bool FLAGS_vw_todo;
extern bool FLAGS_vi;
// -------------------------------------------------------------------------------------
extern std::string FLAGS_ioengine;
extern bool FLAGS_io_uring_poll_mode;
extern s64 FLAGS_io_uring_share_wq;
extern bool FLAGS_raid5;
// -------------------------------------------------------------------------------------
extern bool FLAGS_persist;
extern u64 FLAGS_tx_rate;
extern u64 FLAGS_tmp;
// -------------------------------------------------------------------------------------
extern u64 FLAGS_worker_per_threads;
// -------------------------------------------------------------------------------------
extern bool FLAGS_disable_cross_cores_ut;
// -------------------------------------------------------------------------------------
// YCSB driver knobs (formerly defined in app.cc).
extern u32 FLAGS_ycsb_read_ratio;
