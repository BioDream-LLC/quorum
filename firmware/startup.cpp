// Bare-metal startup for the footprint harness.
//
// No libc, no libstdc++, no newlib. That is the point: every byte `size` reports below belongs to quorum or to code
// written here in full view, so the footprint number cannot quietly include somebody else's runtime.
//
// GCC will still emit calls to `memset` and `memcpy` for aggregate initialisation no matter how freestanding the
// build is, so both are provided. They are the only library functions in the image.

#include <cstddef>
#include <cstdint>

extern "C" {

extern std::uint32_t __data_load;
extern std::uint32_t __data_start;
extern std::uint32_t __data_end;
extern std::uint32_t __bss_start;
extern std::uint32_t __bss_end;
extern std::uint32_t __stack_top;

int main();
void reset_handler();

/// The only two library functions in the image. GCC emits calls to these for struct and array initialisation.
void* memset(void* dst, int value, std::size_t n) {
    auto* p = static_cast<unsigned char*>(dst);
    for (std::size_t i = 0; i < n; ++i) { p[i] = static_cast<unsigned char>(value); }
    return dst;
}

void* memcpy(void* dst, const void* src, std::size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    for (std::size_t i = 0; i < n; ++i) { d[i] = s[i]; }
    return dst;
}

/// Where a real device would drop to its hardwired interlock.
[[noreturn]] void fault_handler() {
    for (;;) {}
}

void reset_handler() {
    // Copy initialised data from flash to RAM.
    const std::uint32_t* src = &__data_load;
    for (std::uint32_t* dst = &__data_start; dst < &__data_end;) { *dst++ = *src++; }

    // Zero bss. Every quorum object lives here: fixed capacity, no allocation, so the RAM cost is known at link
    // time rather than discovered at run time.
    for (std::uint32_t* p = &__bss_start; p < &__bss_end;) { *p++ = 0U; }

    (void)main();
    fault_handler();
}

/// Minimal ARMv8-M vector table. Only the entries a measurement harness can actually reach.
__attribute__((section(".vectors"), used)) void (*const kVectors[])() = {
    reinterpret_cast<void (*)()>(&__stack_top),
    reset_handler,
    fault_handler,  // NMI
    fault_handler,  // HardFault
    fault_handler,  // MemManage
    fault_handler,  // BusFault
    fault_handler,  // UsageFault
};
}
