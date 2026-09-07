#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__SANITIZE_ADDRESS__)
#define SLUICE_FIBER_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLUICE_FIBER_ASAN_ENABLED 1
#endif
#endif

#ifndef SLUICE_FIBER_ASAN_ENABLED
#define SLUICE_FIBER_ASAN_ENABLED 0
#endif

#if defined(__SANITIZE_THREAD__)
#define SLUICE_FIBER_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SLUICE_FIBER_TSAN_ENABLED 1
#endif
#endif

#ifndef SLUICE_FIBER_TSAN_ENABLED
#define SLUICE_FIBER_TSAN_ENABLED 0
#endif

namespace sluice::async::fiber_ctx {

#if defined(__x86_64__) && defined(__linux__)
inline constexpr bool supported = true;
#else
inline constexpr bool supported = false;
#endif

struct Context {
    std::uint64_t rsp = 0;
    std::uint64_t rbp = 0;
    std::uint64_t rip = 0;

#if SLUICE_FIBER_ASAN_ENABLED

    void* asan_fake_stack = nullptr;
    const void* asan_stack_bottom = nullptr;
    std::size_t asan_stack_size = 0;
#endif

#if SLUICE_FIBER_TSAN_ENABLED

    void* sanitizer_fiber = nullptr;
    bool owns_sanitizer_fiber = false;
#endif

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
    Context() noexcept = default;
#if SLUICE_FIBER_TSAN_ENABLED
    ~Context() noexcept;
#else
    ~Context() noexcept = default;
#endif
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
#endif
};

struct Switch {
    Context* old;
    const Context* new_;
};

using Entry = void (*)(Switch* resumed_by, void* user_data);

#if defined(__x86_64__)

Switch* context_switch(Switch* s) noexcept;

#else

inline Switch* context_switch(Switch*) noexcept {
    return nullptr;
}
#endif

[[noreturn]] void context_switch_final(Context& old, const Context& new_) noexcept;

bool init_context(Context& ctx, Entry entry, void* user_data, std::byte* stack_base,
                  std::size_t stack_size) noexcept;

void reset_context(Context& ctx) noexcept;

} // namespace sluice::async::fiber_ctx
