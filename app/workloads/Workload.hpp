#pragma once
// -------------------------------------------------------------------------------------
// A benchmark for the scheduler test bed: schema, initial data and the request
// mix, behind one interface. Picked at boot with workload=<name>.
//
//   setup()    create the tables (B-trees)                       -- in a task
//   load()     fill them with the initial data, on all workers   -- in a task
//   make()     a new request: its type and argument              -- generator thread
//   execute()  run one request                                   -- in a slot task
//
// The type is chosen by the generator, before the request reaches the
// scheduler, so a policy can look at it (e.g. short before long requests).
// Workload-specific boot arguments are read from Params in the constructor.
// -------------------------------------------------------------------------------------
#include "Units.hpp"
// -------------------------------------------------------------------------------------
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
// -------------------------------------------------------------------------------------
namespace leanstore
{
class LeanStore;
}
namespace lsched
{
struct Request;
}
// -------------------------------------------------------------------------------------
namespace workload
{
// -------------------------------------------------------------------------------------
// key=value boot arguments; a workload takes the ones it knows.
class Params
{
   std::map<std::string, std::string> kv_;

  public:
   explicit Params(std::map<std::string, std::string> kv) : kv_(std::move(kv)) {}
   bool has(const std::string& k) const { return kv_.count(k) != 0; }
   std::string str(const std::string& k, const std::string& def)
   {
      auto it = kv_.find(k);
      if (it == kv_.end()) {
         return def;
      }
      std::string v = it->second;
      kv_.erase(it);
      return v;
   }
   double num(const std::string& k, double def)
   {
      return has(k) ? std::stod(str(k, "")) : def;
   }
   // Fails on the first key nobody took.
   void requireEmpty() const
   {
      if (!kv_.empty()) {
         throw std::invalid_argument("unknown boot argument '" + kv_.begin()->first + "'");
      }
   }
};
// -------------------------------------------------------------------------------------
class Workload
{
  public:
   static constexpr int kMaxTypes = 8;

   virtual ~Workload() = default;
   virtual const char* name() const = 0;
   virtual int typeCount() const = 0;
   virtual const char* typeName(int type) const = 0;
   // Buffer pool needed to stay in memory for a run of run_s seconds, as a
   // default for dram_gib (the mem engine aborts if it is too small).
   virtual double dramGib(double run_s) const = 0;
   // Boot arguments as they took effect, for the result's config line.
   virtual std::string describe() const = 0;

   virtual void setup(leanstore::LeanStore& db) = 0;
   virtual void load(leanstore::LeanStore& db, int workers, int tasks_per_worker) = 0;
   virtual void make(lsched::Request& r, std::mt19937_64& rng) = 0;
   virtual void execute(const lsched::Request& r) = 0;
};
// -------------------------------------------------------------------------------------
std::unique_ptr<Workload> makeWorkload(const std::string& name, Params& params);
// -------------------------------------------------------------------------------------
}  // namespace workload
