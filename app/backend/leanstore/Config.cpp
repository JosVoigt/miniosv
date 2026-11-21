// -------------------------------------------------------------------------------------
// Configuration values (formerly gflags). This is the single place to edit them.
//
// Values marked "[was --flag at runtime]" used to be overridden by the fake argv
// that app.cc passed to gflags::ParseCommandLineFlags(); those overrides are now
// baked in here so the build behaves identically without gflags.
// -------------------------------------------------------------------------------------
#include "Config.hpp"
// -------------------------------------------------------------------------------------
std::string FLAGS_free_pages_list_path = "leanstore_free_pages";
// -------------------------------------------------------------------------------------
double FLAGS_dram_gib = 1;
double FLAGS_ssd_gib = 1700;
u32 FLAGS_cool_pct = 10;            // Start cooling pages when <= x% are free
u32 FLAGS_free_pct = 1;
u32 FLAGS_partition_bits = 3;       // [was --partition_bits=3]
u32 FLAGS_pp_threads = 1;           // [was --pp_threads=1]
// -------------------------------------------------------------------------------------
std::string FLAGS_csv_path = "./log";
bool FLAGS_csv_truncate = false;
std::string FLAGS_ssd_path = "/dev/vda";   // [was --ssd_path=/dev/vda] OSv NVMe block device
u32 FLAGS_async_batch_size = 256;
bool FLAGS_trunc = false;           // Truncate file
u32 FLAGS_falloc = 0;               // Preallocate GiB
// -------------------------------------------------------------------------------------
bool FLAGS_print_debug = true;
bool FLAGS_print_tx_console = true;
u32 FLAGS_print_debug_interval_s = 1;
bool FLAGS_print_obj_stats = false;
bool FLAGS_profiling = false;
// -------------------------------------------------------------------------------------
u32 FLAGS_worker_threads = 1;       // [was --worker_threads=1]
u32 FLAGS_worker_tasks = 16;        // [was --worker_tasks=16]
bool FLAGS_nopp = true;             // [was --nopp]
bool FLAGS_pin_threads = false;     // Responsibility of the driver
bool FLAGS_smt = true;              // Simultaneous multithreading
// -------------------------------------------------------------------------------------
bool FLAGS_root = false;
// -------------------------------------------------------------------------------------
u64 FLAGS_backoff_strategy = 0;
// -------------------------------------------------------------------------------------
std::string FLAGS_zipf_path = "/bulk/zipf";
double FLAGS_zipf_factor = 0.0;
double FLAGS_target_gib = 2;        // [was --target_gib=2]
u64 FLAGS_run_for_seconds = 60;     // [was --run_for_seconds=60]
u64 FLAGS_warmup_for_seconds = 10;
// -------------------------------------------------------------------------------------
bool FLAGS_contention_split = true; // [was --contention_split=1]
u64 FLAGS_cm_update_on = 7;         // as exponent of 2
u64 FLAGS_cm_period = 14;           // as exponent of 2
u64 FLAGS_cm_slowpath_threshold = 1;
// -------------------------------------------------------------------------------------
bool FLAGS_xmerge = true;           // [was --xmerge=1]
u64 FLAGS_xmerge_k = 5;
double FLAGS_xmerge_target_pct = 80;
// -------------------------------------------------------------------------------------
u64 FLAGS_backoff = 512;
// -------------------------------------------------------------------------------------
u64 FLAGS_x = 512;
u64 FLAGS_y = 100;
double FLAGS_d = 0.0;
// -------------------------------------------------------------------------------------
bool FLAGS_bulk_insert = false;
// -------------------------------------------------------------------------------------
s64 FLAGS_trace_dt_id = -1;         // Print a stack trace for page reads for this DT ID
s64 FLAGS_trace_trigger_probability = 100;
std::string FLAGS_tag = "";         // appended to each csv line
// -------------------------------------------------------------------------------------
bool FLAGS_out_of_place = false;
bool FLAGS_optimistic_parent_pointer = true;  // [was --optimistic_parent_pointer=1]
// -------------------------------------------------------------------------------------
bool FLAGS_wal = false;
u64 FLAGS_wal_offset_gib = 1;
bool FLAGS_wal_io_hack = false;     // Does not really write logs on SSD
bool FLAGS_wal_fsync = false;
// -------------------------------------------------------------------------------------
bool FLAGS_si = false;
u64 FLAGS_si_refresh_rate = 0;
bool FLAGS_vw = false;              // BTree with SI using versions in WAL
bool FLAGS_vw_todo = false;
bool FLAGS_vi = false;              // BTree with SI using in-place version
// -------------------------------------------------------------------------------------
std::string FLAGS_ioengine = "osv";   // [was --ioengine=osv]
bool FLAGS_io_uring_poll_mode = true;  // enables IORING_SETUP_IOPOLL
s64 FLAGS_io_uring_share_wq = 0;       // enables IORING_SETUP_ATTACH_WQ with a single worker
bool FLAGS_raid5 = false;              // enable RAID 5
// -------------------------------------------------------------------------------------
bool FLAGS_persist = false;
u64 FLAGS_tx_rate = 0;
u64 FLAGS_tmp = 0;
// -------------------------------------------------------------------------------------
u64 FLAGS_worker_per_threads = 100;
// -------------------------------------------------------------------------------------
bool FLAGS_disable_cross_cores_ut = false;
// -------------------------------------------------------------------------------------
u32 FLAGS_ycsb_read_ratio = 100;    // [was --ycsb_read_ratio=100] percentage of reads
