#include "Sched.hpp"
// -------------------------------------------------------------------------------------
#include <stdexcept>
// -------------------------------------------------------------------------------------
namespace lsched
{
// -------------------------------------------------------------------------------------
const char* reqTypeName(ReqType t)
{
   switch (t) {
      case ReqType::Lookup:
         return "lookup";
      case ReqType::Update:
         return "update";
      default:
         return "?";
   }
}
// =====================================================================================
// Queues
// =====================================================================================
// fifo: one global first-come first-served queue; every free slot takes the
// oldest waiting request. Lock-free MPMC ring.
class FifoQueue : public Queue
{
   mutable leanstore::utils::RingBuffer<Request*> ring_;  // size() is not const

  public:
   explicit FifoQueue(size_t capacity) : ring_(capacity) {}
   const char* name() const override { return "fifo"; }
   bool push(Request* r) override { return ring_.try_push(r); }
   Request* pop(int) override
   {
      Request* r = nullptr;
      return ring_.try_pop(r) ? r : nullptr;
   }
   size_t sizeApprox() const override { return ring_.size(); }
};
// =====================================================================================
// Placements
// =====================================================================================
// free: the lowest-numbered core with a free slot. Packs load onto the first
// cores and leaves the high ones idle (good for power, worst for spreading).
class FreeFirstPlacement : public Placement
{
  public:
   const char* name() const override { return "free"; }
   int choose(const Request&, std::vector<Core>& cores) override
   {
      for (auto& c : cores) {
         if (c.tryReserve()) {
            return c.id;
         }
      }
      return -1;
   }
};
// -------------------------------------------------------------------------------------
// free-rr: a core with a free slot, searched round-robin from after the core
// used last. Spreads load evenly over all cores.
class FreeRoundRobinPlacement : public Placement
{
   size_t cursor_ = 0;  // only the generator calls choose()

  public:
   const char* name() const override { return "free-rr"; }
   int choose(const Request&, std::vector<Core>& cores) override
   {
      const size_t n = cores.size();
      for (size_t i = 0; i < n; i++) {
         auto& c = cores[(cursor_ + i) % n];
         if (c.tryReserve()) {
            cursor_ = (c.id + 1) % n;
            return c.id;
         }
      }
      return -1;
   }
};
// -------------------------------------------------------------------------------------
// queue: never place directly; everything goes through the queue and free slots
// pull. Pure work-conserving pull scheduling, no placement decision at all.
class QueueOnlyPlacement : public Placement
{
  public:
   const char* name() const override { return "queue"; }
   int choose(const Request&, std::vector<Core>&) override { return -1; }
};
// =====================================================================================
// Registry
// =====================================================================================
std::unique_ptr<Queue> makeQueue(const std::string& name, size_t capacity)
{
   if (name == "fifo") {
      return std::make_unique<FifoQueue>(capacity);
   }
   throw std::invalid_argument("unknown queue '" + name + "' (have: " + queueNames() + ")");
}
std::string queueNames() { return "fifo"; }
// -------------------------------------------------------------------------------------
std::unique_ptr<Placement> makePlacement(const std::string& name)
{
   if (name == "free") {
      return std::make_unique<FreeFirstPlacement>();
   }
   if (name == "free-rr") {
      return std::make_unique<FreeRoundRobinPlacement>();
   }
   if (name == "queue") {
      return std::make_unique<QueueOnlyPlacement>();
   }
   throw std::invalid_argument("unknown placement '" + name + "' (have: " + placementNames() + ")");
}
std::string placementNames() { return "free free-rr queue"; }
// =====================================================================================
// Scheduler
// =====================================================================================
Scheduler::Scheduler(int cores, int slots, std::unique_ptr<Queue> queue, std::unique_ptr<Placement> placement)
    : slots_(slots), cores_(cores), queue_(std::move(queue)), placement_(std::move(placement))
{
   for (int c = 0; c < cores; c++) {
      cores_[c].id = c;
      cores_[c].slots = slots;
      cores_[c].free_slots = slots;
      // at most `slots` reservations per core, so at most that many in its mailbox
      mailbox_.push_back(std::make_unique<leanstore::utils::RingBuffer<Request*>>(slots < 2 ? 2 : slots));
   }
}
// -------------------------------------------------------------------------------------
bool Scheduler::submit(Request* r)
{
   const int c = placement_->choose(*r, cores_);
   if (c >= 0) {
      cores_[c].placed.fetch_add(1, std::memory_order_relaxed);
      if (!mailbox_[c]->try_push(r)) {
         throw std::logic_error("scheduler: mailbox full despite a reserved slot");
      }
      return true;
   }
   return queue_->push(r);
}
// -------------------------------------------------------------------------------------
Request* Scheduler::next(int core)
{
   Request* r = nullptr;
   // placed here: the slot was reserved by the placement
   if (mailbox_[core]->try_pop(r)) {
      return r;
   }
   // otherwise take a waiting request, under our own reservation. Look before
   // reserving: an idle slot polls here constantly, and reserving writes the
   // core's free_slots line, which the placement reads for every request --
   // with many idle cores that ping-pong slowed the generator down.
   if (queue_->sizeApprox() == 0) {
      return nullptr;
   }
   auto& c = cores_[core];
   if (!c.tryReserve()) {
      return nullptr;  // all slots of this core are spoken for
   }
   r = queue_->pop(core);
   if (!r) {
      c.release();
      return nullptr;
   }
   c.pulled.fetch_add(1, std::memory_order_relaxed);
   return r;
}
// -------------------------------------------------------------------------------------
bool Scheduler::drained() const
{
   if (queue_->sizeApprox() != 0) {
      return false;
   }
   for (size_t c = 0; c < cores_.size(); c++) {
      if (!mailbox_[c]->empty() || !cores_[c].idle()) {
         return false;
      }
   }
   return true;
}
// -------------------------------------------------------------------------------------
}  // namespace lsched
