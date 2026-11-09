/* Validate recovery of a caller's preserved register through the native unwinder.
 * This is a host-only diagnostic control: no private system objects or phone data. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <unwind.h>

#if defined(__aarch64__) || defined(__arm64__)
#define TEST_REGISTER "x19"
static constexpr int kDwarfRegister = 19;
#elif defined(__x86_64__)
#define TEST_REGISTER "r12"
static constexpr int kDwarfRegister = 12;
#else
#error Unsupported host architecture for this diagnostic
#endif

struct Probe {
 uintptr_t expectedIP;
 uintptr_t recovered;
 unsigned matches;
};

static _Unwind_Reason_Code collect(struct _Unwind_Context *context, void *argument) {
 auto *probe = static_cast<Probe *>(argument);
 if (static_cast<uintptr_t>(_Unwind_GetIP(context)) == probe->expectedIP) {
  probe->recovered = static_cast<uintptr_t>(_Unwind_GetGR(context, kDwarfRegister));
  ++probe->matches;
 }
 return _URC_NO_REASON;
}

__attribute__((noinline)) static bool observe(uintptr_t expected) {
 // Force the callee to use the same register for a different value. The
 // unwinder must recover the caller's saved value, not the callee's live one.
 register uintptr_t overwritten asm(TEST_REGISTER) = expected ^ UINT64_C(0x5555555555555555);
 asm volatile("" : "+r"(overwritten) : : "memory");
 Probe probe{reinterpret_cast<uintptr_t>(__builtin_return_address(0)), 0, 0};
 _Unwind_Backtrace(collect, &probe);
 asm volatile("" : "+r"(overwritten) : : "memory");
 return probe.matches == 1 && probe.recovered == expected && overwritten != expected;
}

__attribute__((noinline)) static bool caller(uintptr_t value) {
 register uintptr_t preserved asm(TEST_REGISTER) = value;
 asm volatile("" : "+r"(preserved) : : "memory");
 bool ok = observe(value);
 asm volatile("" : "+r"(preserved) : : "memory");
 return ok && preserved == value;
}

int main() {
 const uintptr_t first = UINT64_C(0x123456789abcdef0);
 const uintptr_t second = UINT64_C(0x0fedcba987654321);
 assert(caller(first));
 assert(caller(second));
 std::puts("PASS: native unwinder recovered each caller's preserved register");
}
