#pragma once
// -------------------------------------------------------------------------------------
// Latency statistics for the scheduler prototype.
//
// One CoreStats per core, written only by that core's slots (they run
// cooperatively on one executor thread, so no synchronisation is needed) and
// merged after the run.
// -------------------------------------------------------------------------------------
#include "Sched.hpp"
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <array>
#include <cstdint>
// -------------------------------------------------------------------------------------
namespace lsched
{
// -------------------------------------------------------------------------------------
// Log-linear histogram over nanoseconds: exact below 16 ns, then 8 sub-buckets
// per power of two (<= 12.5 % relative error).
class Histogram
{
  public:
   static constexpr int kBuckets = 16 + 60 * 8;

   void add(uint64_t ns)
   {
      buckets_[index(ns)]++;
      count_++;
      sum_ += ns;
      max_ = std::max(max_, ns);
   }
   void merge(const Histogram& o)
   {
      for (int i = 0; i < kBuckets; i++) {
         buckets_[i] += o.buckets_[i];
      }
      count_ += o.count_;
      sum_ += o.sum_;
      max_ = std::max(max_, o.max_);
   }
   uint64_t count() const { return count_; }
   uint64_t max() const { return max_; }
   double mean() const { return count_ ? double(sum_) / count_ : 0; }
   // Upper bound of the bucket holding the q-quantile.
   uint64_t quantile(double q) const
   {
      if (count_ == 0) {
         return 0;
      }
      const uint64_t rank = std::max<uint64_t>(1, uint64_t(q * count_ + 0.5));
      uint64_t seen = 0;
      for (int i = 0; i < kBuckets; i++) {
         seen += buckets_[i];
         if (seen >= rank) {
            return std::min(upper(i), max_);
         }
      }
      return max_;
   }

  private:
   static int index(uint64_t v)
   {
      if (v < 16) {
         return int(v);
      }
      const int msb = 63 - __builtin_clzll(v);         // >= 4
      const int sub = int((v >> (msb - 3)) & 7);       // next three bits
      return std::min(16 + (msb - 4) * 8 + sub, kBuckets - 1);
   }
   static uint64_t upper(int i)
   {
      if (i < 16) {
         return uint64_t(i);
      }
      const int msb = (i - 16) / 8 + 4;
      const int sub = (i - 16) % 8;
      return ((uint64_t(8 + sub + 1)) << (msb - 3)) - 1;
   }

   std::array<uint64_t, kBuckets> buckets_{};
   uint64_t count_ = 0;
   uint64_t sum_ = 0;
   uint64_t max_ = 0;
};
// -------------------------------------------------------------------------------------
enum Metric { kTotal = 0, kWait = 1, kService = 2, kMetrics = 3 };
inline const char* metricName(int m)
{
   static const char* n[] = {"total", "wait", "service"};
   return n[m];
}
// -------------------------------------------------------------------------------------
struct alignas(64) CoreStats {
   std::array<std::array<Histogram, kMetrics>, size_t(ReqType::COUNT)> lat;
   uint64_t completed = 0;
   uint64_t busy_tsc = 0;  // sum of service times: how loaded the core was

   void record(const Request& r, double ns_per_tick)
   {
      auto& h = lat[size_t(r.type)];
      h[kTotal].add(uint64_t((r.t_done - r.t_arrive) * ns_per_tick));
      h[kWait].add(uint64_t((r.t_start - r.t_arrive) * ns_per_tick));
      h[kService].add(uint64_t((r.t_done - r.t_start) * ns_per_tick));
   }
};
// -------------------------------------------------------------------------------------
}  // namespace lsched
