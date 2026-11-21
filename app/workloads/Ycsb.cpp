// YCSB: one table of 8-byte keys and 120-byte records; lookups and updates.
//
// Boot arguments: data_gib=1 (half of it tuple payload, as LeanStore's YCSB),
// read=100 (% lookups), zipf=0 (key skew).
#include "Workload.hpp"
#include "sched/Sched.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <cstdio>
#include <memory>
// -------------------------------------------------------------------------------------
namespace workload
{
namespace
{
using Key = u64;
using Payload = leanstore::BytesPayload<120>;

class Ycsb : public Workload
{
   enum Type : uint8_t { kLookup = 0, kUpdate = 1 };

   double data_gib_;
   int read_;
   double zipf_theta_;
   u64 tuples_;
   std::unique_ptr<leanstore::BTreeInterface<Key, Payload>> table_;
   std::unique_ptr<leanstore::utils::ScrambledZipfGenerator> zipf_;  // generator thread only

  public:
   explicit Ycsb(Params& p)
       : data_gib_(p.num("data_gib", 1)), read_(int(p.num("read", 100))), zipf_theta_(p.num("zipf", 0))
   {
      tuples_ = u64(data_gib_ * 1024.0 * 1024.0 * 1024.0 / 2.0 / (sizeof(Key) + sizeof(Payload)));
   }
   const char* name() const override { return "ycsb"; }
   int typeCount() const override { return 2; }
   const char* typeName(int t) const override { return t == kLookup ? "lookup" : "update"; }
   // pages hold ~0.75x data_gib of tuples; room for inner nodes; updates don't grow it
   double dramGib(double) const override { return data_gib_ * 1.5 + 0.5; }
   std::string describe() const override
   {
      char b[96];
      snprintf(b, sizeof(b), "data_gib=%.2f,read=%d,zipf=%.2f,tuples=%lu", data_gib_, read_, zipf_theta_,
               (unsigned long)tuples_);
      return b;
   }

   void setup(leanstore::LeanStore& db) override
   {
      auto& btree = db.registerBTreeLL("ycsb", "y");
      table_ = std::make_unique<leanstore::BTreeVSAdapter<Key, Payload>>(btree);
      zipf_ = std::make_unique<leanstore::utils::ScrambledZipfGenerator>(0, tuples_, zipf_theta_);
   }

   void load(leanstore::LeanStore&, int, int tasks_per_worker) override
   {
      printf("ycsb: loading %lu tuples\n", (unsigned long)tuples_);
      std::atomic<u64> next_block{0};
      const u64 block = 1 << 16;
      auto& table = *table_;
      const u64 n = tuples_;
      auto insert = [&]() {
         for (;;) {
            const u64 b = next_block.fetch_add(block, std::memory_order_relaxed);
            if (b >= n) {
               break;
            }
            const u64 e = std::min(b + block, n);
            for (u64 k = b; k < e; k++) {
               Payload payload;
               leanstore::utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(Payload));
               table.insert(k, payload);
            }
         }
      };
      mean::task::parallelFor(BlockedRange(0, (u64)1), insert, tasks_per_worker, -1, false);
   }

   void make(lsched::Request& r, std::mt19937_64& rng) override
   {
      r.type = std::uniform_int_distribution<int>(0, 99)(rng) < read_ ? kLookup : kUpdate;
      r.arg = zipf_->rand();
   }

   void execute(const lsched::Request& r) override
   {
      if (r.type == kLookup) {
         Payload result;
         table_->lookup(r.arg, result);
      } else {
         Payload payload;
         leanstore::utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(Payload));
         table_->update(r.arg, payload);
      }
   }
};
}  // namespace
// -------------------------------------------------------------------------------------
std::unique_ptr<Workload> makeYcsb(Params& p)
{
   return std::make_unique<Ycsb>(p);
}
}  // namespace workload
