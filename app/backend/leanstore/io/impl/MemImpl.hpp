#pragma once
// -------------------------------------------------------------------------------------
// The "mem" I/O engine: no storage device at all.
//
// For in-memory runs (data set smaller than the buffer pool) nothing should ever
// reach the SSD after the load. This engine gives LeanStore its channels and
// I/O buffers without probing any NVMe controller, and aborts on the first real
// read or write -- so a run that silently starts evicting fails loudly instead
// of mixing I/O into a CPU-scheduling measurement.
// -------------------------------------------------------------------------------------
#include "./Osv.hpp"  // OsvIoReq, the request payload the RAID layer carries
#include "../IoAbstraction.hpp"
// -------------------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
// -------------------------------------------------------------------------------------
namespace mean
{
class MemChannel
{
  public:
   void _push(RaidRequest<OsvIoReq>* req)
   {
      printf("mem engine: I/O requested (%s, offset %lu, %lu bytes) -- the data set does not fit "
             "the buffer pool; raise dram_gib or lower data_gib\n",
             req->base.type == IoRequestType::Read ? "read" : "write", (unsigned long)req->base.offset,
             (unsigned long)req->base.len);
      abort();
   }
   void pushBlocking(IoRequestType, char*, s64, u64, bool)
   {
      printf("mem engine: blocking I/O requested\n");
      abort();
   }
   int _poll(int) { return 0; }
   void _printSpecializedCounters(std::ostream& ss) { ss << "mem: "; }
};
// -------------------------------------------------------------------------------------
class MemEnv
{
   std::vector<std::unique_ptr<MemChannel>> channels;

  public:
   void init(IoOptions options)
   {
      const int n = options.channelCount > 0 ? options.channelCount : 1;
      for (int i = 0; i < n; i++) {
         channels.push_back(std::make_unique<MemChannel>());
      }
      printf("mem engine: %d channels, no storage device\n", n);
   }
   MemChannel& getIoChannel(int channel) { return *channels.at(channel); }
   int channelCount() { return channels.size(); }
   int deviceCount() { return 1; }
   u64 storageSize() { return 1ull << 40; }
   DeviceInformation getDeviceInfo()
   {
      DeviceInformation d;
      d.devices.resize(1);
      d.devices[0].id = 0;
      return d;
   }
   void* allocIoMemory(size_t size, size_t align)
   {
      if (align < sizeof(void*)) {
         align = sizeof(void*);
      }
      const size_t rounded = (size + align - 1) & ~(align - 1);
      void* p = std::aligned_alloc(align, rounded);
      if (!p) {
         throw std::runtime_error("mem engine: out of memory");
      }
      memset(p, 0, rounded);
      return p;
   }
   void* allocIoMemoryChecked(size_t size, size_t align) { return allocIoMemory(size, align); }
   void freeIoMemory(void* ptr, size_t) { std::free(ptr); }
};
}  // namespace mean
