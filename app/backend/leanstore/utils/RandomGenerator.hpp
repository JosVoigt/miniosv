#pragma once
#include "Exceptions.hpp"
// -------------------------------------------------------------------------------------
#include <atomic>
#include "Time.hpp"
#include <new>
#include <random>
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace utils
{
class MersenneTwister
{
  private:
   static const int NN = 312;
   static const int MM = 156;
   static const uint64_t MATRIX_A = 0xB5026F5AA96619E9ULL;
   static const uint64_t UM = 0xFFFFFFFF80000000ULL;
   static const uint64_t LM = 0x7FFFFFFFULL;
   uint64_t mt[NN];
   int mti;
   void init(uint64_t seed);

  public:
   MersenneTwister(uint64_t seed = 19650218ULL);
   uint64_t rnd();
};
}  // namespace utils
}  // namespace leanstore
// -------------------------------------------------------------------------------------
// One generator of each kind per thread, as upstream (static thread_local).
// This port had dropped the thread_local, so all executors shared one
// generator and raced on its state: at 26 workers the TPC-C loader drew the
// same customer names twice (duplicate key, abort in BTreeLL::insert).
// The generators are built on first use in constinit TLS storage (no dynamic
// TLS initialisation, like jumpmu's context) and seeded per thread, so that
// threads don't all draw the same sequence. mt_generator and
// random_generator stay as names: stateless forwarders to this thread's.
namespace leanstore
{
namespace utils
{
namespace detail
{
struct TlsRng {
   alignas(MersenneTwister) unsigned char mt[sizeof(MersenneTwister)];
   alignas(std::mt19937) unsigned char std[sizeof(std::mt19937)];
   bool ready;
};
inline constinit thread_local TlsRng tls_rng{};
inline void tlsRngInit()
{
   const uint64_t seed = mean::readTSC() ^ reinterpret_cast<uintptr_t>(&tls_rng);
   new (tls_rng.mt) MersenneTwister(seed);
   new (tls_rng.std) std::mt19937(uint32_t(seed ^ (seed >> 32)));
   tls_rng.ready = true;
}
inline MersenneTwister& mtGen()
{
   if (__builtin_expect(!tls_rng.ready, 0)) {
      tlsRngInit();
   }
   return *std::launder(reinterpret_cast<MersenneTwister*>(tls_rng.mt));
}
inline std::mt19937& stdGen()
{
   if (__builtin_expect(!tls_rng.ready, 0)) {
      tlsRngInit();
   }
   return *std::launder(reinterpret_cast<std::mt19937*>(tls_rng.std));
}
struct MtForward {
   uint64_t rnd() { return mtGen().rnd(); }
};
struct StdForward {  // a UniformRandomBitGenerator
   using result_type = std::mt19937::result_type;
   static constexpr result_type min() { return std::mt19937::min(); }
   static constexpr result_type max() { return std::mt19937::max(); }
   result_type operator()() { return stdGen()(); }
   void seed(result_type s) { stdGen().seed(s); }
};
}  // namespace detail
}  // namespace utils
}  // namespace leanstore
static leanstore::utils::detail::MtForward mt_generator;
static leanstore::utils::detail::StdForward random_generator;
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace utils
{
// -------------------------------------------------------------------------------------
class RandomGenerator
{
  public:
   // ATTENTION: open interval [min, max)
   static u64 getRandU64(u64 min, u64 max)
   {
      u64 rand = min + (mt_generator.rnd() % (max - min));
      assert(rand < max);
      assert(rand >= min);
      return rand;
   }
   static u64 getRandU64() { return mt_generator.rnd(); }
   static u64 getRandU64STD(u64 min, u64 max)
   {
      std::uniform_int_distribution<u64> distribution(min, max - 1);
      return distribution(random_generator);
   }
   template <typename T>
   static inline T getRand(T min, T max)
   {
      u64 rand = getRandU64(min, max);
      return static_cast<T>(rand);
   }
   static void getRandString(u8* dst, u64 size);
};
// -------------------------------------------------------------------------------------
}  // namespace utils
}  // namespace leanstore
   // -------------------------------------------------------------------------------------
