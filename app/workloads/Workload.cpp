#include "Workload.hpp"
// -------------------------------------------------------------------------------------
namespace workload
{
std::unique_ptr<Workload> makeYcsb(Params& p);
std::unique_ptr<Workload> makeTpcc(Params& p);
// -------------------------------------------------------------------------------------
std::unique_ptr<Workload> makeWorkload(const std::string& name, Params& params)
{
   if (name == "ycsb") {
      return makeYcsb(params);
   }
   if (name == "tpcc") {
      return makeTpcc(params);
   }
   throw std::invalid_argument("unknown workload '" + name + "' (have: ycsb tpcc)");
}
}  // namespace workload
