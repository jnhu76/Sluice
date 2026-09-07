#include <sluice/async/fiber_ctx.hpp>

#include <cstdlib>
#include <cstdint>

#if SLUICE_FIBER_ASAN_ENABLED
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                     std::size_t* size_old);
}
#endif

#if SLUICE_FIBER_TSAN_ENABLED
extern "C" {
void* __tsan_get_current_fiber();
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
}
#endif

namespace sluice::async::fiber_ctx {

#if SLUICE_FIBER_TSAN_ENABLED
namespace {

void release_sanitizer_fiber(Context& ctx) noexcept {
    if (ctx.owns_sanitizer_fiber && ctx.sanitizer_fiber != nullptr) {
        __tsan_destroy_fiber(ctx.sanitizer_fiber);
    }
    ctx.sanitizer_fiber = nullptr;
    ctx.owns_sanitizer_fiber = false;
}

} // namespace

Context::~Context() noexcept {
    release_sanitizer_fiber(*this);
}
#endif

void reset_context(Context& ctx) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    ctx.asan_fake_stack = nullptr;
    ctx.asan_stack_bottom = nullptr;
    ctx.asan_stack_size = 0;
#endif
#if SLUICE_FIBER_TSAN_ENABLED
    release_sanitizer_fiber(ctx);
#endif
    ctx.rsp = 0;
    ctx.rbp = 0;
    ctx.rip = 0;
}

extern "C" void fiber_entry_trampoline();

#if defined(__x86_64__)

#if SLUICE_FIBER_ASAN_ENABLED
__attribute__((no_sanitize("address")))
#endif
void finish_asan_switch(Switch* resumed_by, void* fake_stack) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    const void* old_bottom = nullptr;
    std::size_t old_size = 0;
    __sanitizer_finish_switch_fiber(fake_stack, &old_bottom, &old_size);
    resumed_by->old->asan_stack_bottom = old_bottom;
    resumed_by->old->asan_stack_size = old_size;
#else
    (void)resumed_by;
    (void)fake_stack;
#endif
}

#if SLUICE_FIBER_ASAN_ENABLED
__attribute__((no_sanitize("address")))
#endif
extern "C" void fiber_entry_trampoline_bridge(Switch* resumed_by, void* user_data, Entry entry) {
#if SLUICE_FIBER_ASAN_ENABLED
    finish_asan_switch(resumed_by, resumed_by->new_->asan_fake_stack);
#endif
    entry(resumed_by, user_data);
}

asm(".text\n"
    ".globl fiber_entry_trampoline\n"
    ".type fiber_entry_trampoline, @function\n"
    "fiber_entry_trampoline:\n"
    "  popq %rdx\n"
    "  addq $8, %rsp\n"
    "  movq %rsi, %rdi\n"
    "  popq %rsi\n"
    "  callq fiber_entry_trampoline_bridge\n"
    "  ud2\n"
    ".size fiber_entry_trampoline, .-fiber_entry_trampoline\n");

namespace {

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
Switch* native_context_switch(Switch* s) noexcept {
    Switch* resumed_by;
    __asm__ volatile("movq 0(%%rsi), %%rax\n\t"
                     "movq 8(%%rsi), %%rcx\n\t"
                     "leaq 0f(%%rip), %%rdx\n\t"
                     "movq %%rsp, 0(%%rax)\n\t"
                     "movq %%rbp, 8(%%rax)\n\t"
                     "movq %%rdx, 16(%%rax)\n\t"
                     "movq 0(%%rcx), %%rsp\n\t"
                     "movq 8(%%rcx), %%rbp\n\t"
                     "jmpq *16(%%rcx)\n\t"
                     "0:\n\t"
                     : "=S"(resumed_by)
                     : "S"(s)
                     : "rax", "rcx", "rdx", "rbx", "rdi", "r8", "r9", "r10", "r11", "r12", "r13",
                       "r14", "r15", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st",
                       "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)", "cc", "memory"

    );
    return resumed_by;
}

#if SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
void prepare_tsan_switch(Switch* s) noexcept {
#if SLUICE_FIBER_TSAN_ENABLED

    s->old->sanitizer_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(s->new_->sanitizer_fiber, 0);
#else
    (void)s;
#endif
}

} // namespace

#if SLUICE_FIBER_TSAN_ENABLED
__attribute__((no_sanitize("thread")))
#endif
Switch* context_switch(Switch* s) noexcept {
#if SLUICE_FIBER_ASAN_ENABLED
    __sanitizer_start_switch_fiber(&s->old->asan_fake_stack, s->new_->asan_stack_bottom,
                                   s->new_->asan_stack_size);
#endif
    prepare_tsan_switch(s);
    Switch* resumed_by = native_context_switch(s);
#if SLUICE_FIBER_ASAN_ENABLED
    finish_asan_switch(resumed_by, s->old->asan_fake_stack);
#endif
    return resumed_by;
}

#if SLUICE_FIBER_ASAN_ENABLED || SLUICE_FIBER_TSAN_ENABLED
__attribute__((disable_sanitizer_instrumentation))
#endif
void context_switch_final(Context& old, const Context& new_) noexcept {

    Switch s{&old, &new_};
#if SLUICE_FIBER_ASAN_ENABLED
    __sanitizer_start_switch_fiber(nullptr, new_.asan_stack_bottom, new_.asan_stack_size);
#endif
    prepare_tsan_switch(&s);
    (void)native_context_switch(&s);
    std::abort();
}

bool init_context(Context& ctx, Entry entry, void* user_data, std::byte* stack_base,
                  std::size_t stack_size) noexcept {
    if (entry == nullptr || stack_base == nullptr || stack_size < 64) {
        return false;
    }
#if SLUICE_FIBER_TSAN_ENABLED
    release_sanitizer_fiber(ctx);
    ctx.sanitizer_fiber = __tsan_create_fiber(0);
    if (ctx.sanitizer_fiber == nullptr) {
        return false;
    }
    ctx.owns_sanitizer_fiber = true;
#endif
#if SLUICE_FIBER_ASAN_ENABLED
    ctx.asan_fake_stack = nullptr;
    ctx.asan_stack_bottom = stack_base;
    ctx.asan_stack_size = stack_size;
#endif

    auto top = reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<std::uintptr_t>(0xF);
    auto* p = reinterpret_cast<std::uint64_t*>(top);
    p[-1] = reinterpret_cast<std::uint64_t>(user_data);
    p[-2] = 0;
    p[-3] = reinterpret_cast<std::uint64_t>(entry);

    ctx.rsp = reinterpret_cast<std::uint64_t>(&p[-3]);
    ctx.rbp = 0;
    ctx.rip = reinterpret_cast<std::uint64_t>(&fiber_entry_trampoline);
    return true;
}

#else

[[noreturn]] void context_switch_final(Context&, const Context&) noexcept {
    std::abort();
}

bool init_context(Context&, Entry, void*, std::byte*, std::size_t) noexcept {
    return false;
}

extern "C" void fiber_entry_trampoline() {}

#endif

} // namespace sluice::async::fiber_ctx
