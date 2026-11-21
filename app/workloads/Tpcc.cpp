// TPC-C without think times, as LeanStore runs it (upstream branch io,
// frontend/tpc-c): the 9 tables, the loader and the five transactions are
// upstream's, in tpcc/*.hpp. What is ours: the generator picks the
// transaction type (so the scheduler knows it) and the warehouse; a slot runs
// that transaction.
//
// Boot arguments:
//   warehouses=4             scale factor W (~100 MB of initial data each)
//   mix=45,43,4,4,4          weights of new-order, payment, order-status,
//                            delivery, stock-level (the spec's minimum mix;
//                            upstream's tx() uses the same)
//
// Not done (as upstream): think and keying times, ACID tests, durability;
// all warehouses are reachable from every core.
#include "Workload.hpp"
#include "sched/Sched.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/sync-primitives/JumpMU.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "Time.hpp"
// -------------------------------------------------------------------------------------
#include <array>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
// -------------------------------------------------------------------------------------
// The upstream sources rely on these being in scope.
using std::atomic;
using std::cout;
using std::endl;
using std::function;
using std::string;
using namespace leanstore;
// -------------------------------------------------------------------------------------
#include "tpcc/types.hpp"
#include "tpcc/adapter.hpp"
#include "tpcc/schema.hpp"
// -------------------------------------------------------------------------------------
namespace
{
// upstream: DEFINE_bool(tpcc_remove, true, "") in tpcc.cpp -- delivery removes
// the delivered new-order rows
bool FLAGS_tpcc_remove = true;

LeanStoreAdapter<warehouse_t> warehouse;
LeanStoreAdapter<district_t> district;
LeanStoreAdapter<customer_t> customer;
LeanStoreAdapter<customer_wdl_t> customerwdl;
LeanStoreAdapter<history_t> history;
LeanStoreAdapter<neworder_t> neworder;
LeanStoreAdapter<order_t> order;
LeanStoreAdapter<order_wdc_t> order_wdc;
LeanStoreAdapter<orderline_t> orderline;
LeanStoreAdapter<item_t> item;
LeanStoreAdapter<stock_t> stock;

// upstream's "dirty include" (tpcc.cpp): it uses the tables above
#include "tpcc/tpcc_workload.hpp"
}  // namespace
// -------------------------------------------------------------------------------------
namespace workload
{
namespace
{
class Tpcc : public Workload
{
   enum Type : uint8_t { kNewOrder = 0, kPayment, kOrderStatus, kDelivery, kStockLevel, kTypes };
   static constexpr const char* kNames[kTypes] = {"neworder", "payment", "orderstatus", "delivery", "stocklevel"};

   int warehouses_;
   std::array<int, kTypes> mix_{45, 43, 4, 4, 4};
   std::array<int, kTypes> cdf_{};
   int total_ = 0;
   std::atomic<u64> aborts_{0};

  public:
   explicit Tpcc(Params& p) : warehouses_(int(p.num("warehouses", 4)))
   {
      if (warehouses_ < 1) {
         throw std::invalid_argument("warehouses must be >= 1");
      }
      if (p.has("mix")) {
         std::stringstream ss(p.str("mix", ""));
         std::string w;
         for (int t = 0; t < kTypes; t++) {
            if (!std::getline(ss, w, ',')) {
               throw std::invalid_argument("mix needs 5 weights: neworder,payment,orderstatus,delivery,stocklevel");
            }
            mix_[t] = std::stoi(w);
         }
      }
      for (int t = 0; t < kTypes; t++) {
         total_ += mix_[t];
         cdf_[t] = total_;
      }
      if (total_ <= 0) {
         throw std::invalid_argument("mix weights sum to 0");
      }
   }
   const char* name() const override { return "tpcc"; }
   int typeCount() const override { return kTypes; }
   const char* typeName(int t) const override { return kNames[t]; }
   // Initial data ~0.15 GiB/W in pages (to be measured); new-order and
   // payment keep inserting (orders, order lines, history), so leave room.
   double dramGib(double run_s) const override { return 0.25 * warehouses_ + 2 + 0.5 * run_s; }
   std::string describe() const override
   {
      char b[96];
      snprintf(b, sizeof(b), "warehouses=%d,mix=%d/%d/%d/%d/%d", warehouses_, mix_[0], mix_[1], mix_[2], mix_[3],
               mix_[4]);
      return b;
   }

   void setup(LeanStore& db) override
   {
      warehouseCount = warehouses_;
      warehouse = LeanStoreAdapter<warehouse_t>(db, "warehouse", "w");
      district = LeanStoreAdapter<district_t>(db, "district", "d");
      customer = LeanStoreAdapter<customer_t>(db, "customer", "c");
      customerwdl = LeanStoreAdapter<customer_wdl_t>(db, "customerwdl", "k");
      history = LeanStoreAdapter<history_t>(db, "history", "h");
      neworder = LeanStoreAdapter<neworder_t>(db, "neworder", "n");
      order = LeanStoreAdapter<order_t>(db, "order", "o");
      order_wdc = LeanStoreAdapter<order_wdc_t>(db, "order_wdc", "r");
      orderline = LeanStoreAdapter<orderline_t>(db, "orderline", "l");
      item = LeanStoreAdapter<item_t>(db, "item", "i");
      stock = LeanStoreAdapter<stock_t>(db, "stock", "s");
   }

   void load(LeanStore&, int, int) override
   {
      printf("tpcc: loading %d warehouses\n", warehouses_);
      // as upstream: items and warehouses first, then per warehouse in parallel
      loadItem();
      loadWarehouse();
      std::atomic<int> next{1};
      const int w_max = warehouses_;
      auto load_fun = [&]() {
         for (;;) {
            const int w_id = next.fetch_add(1, std::memory_order_relaxed);
            if (w_id > w_max) {
               break;
            }
            loadStock(w_id);
            loadDistrinct(w_id);
            for (Integer d_id = 1; d_id <= 10; d_id++) {
               loadCustomer(w_id, d_id);
               loadOrders(w_id, d_id);
            }
         }
      };
      mean::task::parallelFor(BlockedRange(0, (u64)1), load_fun, 1, -1, false);
   }

   void make(lsched::Request& r, std::mt19937_64& rng) override
   {
      const int x = std::uniform_int_distribution<int>(0, total_ - 1)(rng);
      int t = 0;
      while (x >= cdf_[t]) {
         t++;
      }
      r.type = uint8_t(t);
      r.arg = std::uniform_int_distribution<int>(1, warehouses_)(rng);
   }

   void execute(const lsched::Request& r) override
   {
      const Integer w_id = Integer(r.arg);
      jumpmuTry()
      {
         switch (r.type) {
            case kNewOrder:
               newOrderRnd(w_id);
               break;
            case kPayment:
               paymentRnd(w_id);
               break;
            case kOrderStatus:
               orderStatusRnd(w_id);
               break;
            case kDelivery:
               deliveryRnd(w_id);
               break;
            default:
               stockLevelRnd(w_id);
               break;
         }
      }
      jumpmuCatch() { aborts_.fetch_add(1, std::memory_order_relaxed); }
   }
};
}  // namespace
// -------------------------------------------------------------------------------------
std::unique_ptr<Workload> makeTpcc(Params& p)
{
   return std::make_unique<Tpcc>(p);
}
}  // namespace workload
