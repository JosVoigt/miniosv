#pragma once
// -------------------------------------------------------------------------------------
// Request scheduling for the LeanStore prototype.
//
// A request (one YCSB lookup or update, later scans etc.) arrives from the load
// generator and has to end up on a core. Two separate decisions, each behind
// its own interface so they can be swapped independently by name:
//
//   Placement  where does a request go?   choose() picks a core with a free
//              slot and reserves that slot, or says "none free".
//   Queue      what waits, and in what order, when no core is free? Free
//              slots pull from it.
//
// A core is one LeanStore executor (one pinned thread). It runs `slots` worker
// tasks; a slot holds at most one request at a time, so slots = 1 means "one
// request per core at a time" and a free slot is a free core.
//
// Flow:
//   generator  -> Scheduler::submit(r)
//                   placement.choose(r) = c   -> mailbox[c]   (slot reserved)
//                   none free                 -> queue
//   slot on c  -> Scheduler::next(c)
//                   mailbox[c] first, else reserve own slot and queue.pop(c)
//              -> run r -> Scheduler::finished(c)
//
// Every request runs under exactly one reservation (made by the placement or
// by the slot itself), released in finished(); so a core never runs more than
// `slots` requests at once.
//
// Adding a policy: implement Queue or Placement below, add it to the registry
// in Sched.cpp, select it with the boot argument queue=<name> / place=<name>.
// -------------------------------------------------------------------------------------
#include "leanstore/utils/RingBuffer.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
// -------------------------------------------------------------------------------------
namespace lsched
{
// -------------------------------------------------------------------------------------
enum class ReqType : uint8_t { Lookup = 0, Update = 1, COUNT };
const char* reqTypeName(ReqType t);
// -------------------------------------------------------------------------------------
struct Request {
   ReqType type = ReqType::Lookup;
   uint64_t key = 0;
   // TSC timestamps: arrival at the scheduler, start and end of service
   uint64_t t_arrive = 0;
   uint64_t t_start = 0;
   uint64_t t_done = 0;
   int core = -1;  // the core it ran on
};
// -------------------------------------------------------------------------------------
// What the placement sees of a core.
struct alignas(64) Core {
   int id = 0;
   int slots = 0;
   std::atomic<int> free_slots{0};  // slots without a reservation
   std::atomic<uint64_t> placed{0};  // requests the placement put here
   std::atomic<uint64_t> pulled{0};  // requests a slot here took from the queue

   bool tryReserve()
   {
      int f = free_slots.load(std::memory_order_relaxed);
      while (f > 0) {
         if (free_slots.compare_exchange_weak(f, f - 1, std::memory_order_acq_rel)) {
            return true;
         }
      }
      return false;
   }
   void release() { free_slots.fetch_add(1, std::memory_order_acq_rel); }
   bool idle() const { return free_slots.load(std::memory_order_relaxed) == slots; }
};
// -------------------------------------------------------------------------------------
// Which core a request lands on.
class Placement
{
  public:
   virtual ~Placement() = default;
   virtual const char* name() const = 0;
   // Return a core and reserve one of its slots (Core::tryReserve), or -1 when
   // no core may take the request now (it then waits in the queue).
   virtual int choose(const Request& r, std::vector<Core>& cores) = 0;
};
// -------------------------------------------------------------------------------------
// The order in which waiting requests are served.
class Queue
{
  public:
   virtual ~Queue() = default;
   virtual const char* name() const = 0;
   virtual bool push(Request* r) = 0;      // false when full
   virtual Request* pop(int core) = 0;     // nullptr when empty; `core` asks
   virtual size_t sizeApprox() const = 0;
};
// -------------------------------------------------------------------------------------
std::unique_ptr<Queue> makeQueue(const std::string& name, size_t capacity);
std::unique_ptr<Placement> makePlacement(const std::string& name);
std::string queueNames();      // for error messages
std::string placementNames();
// -------------------------------------------------------------------------------------
class Scheduler
{
  public:
   Scheduler(int cores, int slots, std::unique_ptr<Queue> queue, std::unique_ptr<Placement> placement);

   // Generator side. False when the request could not be queued (queue full).
   bool submit(Request* r);
   // Slot side: the next request for a free slot on `core`, or nullptr.
   Request* next(int core);
   // Slot side: the slot on `core` is done with its request.
   void finished(int core) { cores_[core].release(); }
   // True when nothing is queued, placed or running.
   bool drained() const;

   const Queue& queue() const { return *queue_; }
   const Placement& placement() const { return *placement_; }
   std::vector<Core>& cores() { return cores_; }
   int slots() const { return slots_; }

  private:
   int slots_;
   std::vector<Core> cores_;
   std::vector<std::unique_ptr<leanstore::utils::RingBuffer<Request*>>> mailbox_;
   std::unique_ptr<Queue> queue_;
   std::unique_ptr<Placement> placement_;
};
// -------------------------------------------------------------------------------------
}  // namespace lsched
