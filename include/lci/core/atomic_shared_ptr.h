#pragma once

#include <atomic>
#include <memory>
#include <version>

// ---------------------------------------------------------------------------
// Deprecation-suppression macros — used only in the libc++ fallback branch
// but defined unconditionally so the header is well-formed everywhere.
// ---------------------------------------------------------------------------
#if defined(__clang__)
#define LCI_DIAG_PUSH_IGNORE_DEPRECATED                                \
    _Pragma("clang diagnostic push")                                   \
    _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define LCI_DIAG_POP _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define LCI_DIAG_PUSH_IGNORE_DEPRECATED                               \
    _Pragma("GCC diagnostic push")                                    \
    _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define LCI_DIAG_POP _Pragma("GCC diagnostic pop")
#else
#define LCI_DIAG_PUSH_IGNORE_DEPRECATED
#define LCI_DIAG_POP
#endif

namespace lci {

/// std::atomic<std::shared_ptr<T>> is C++20, but libc++ (macOS/AppleClang)
/// has not implemented it (the _Atomic intrinsic rejects the non-trivially-
/// copyable shared_ptr). Where the native type exists (libstdc++, MSVC STL)
/// we use it directly; on libc++ we fall back to the std::atomic_*_explicit
/// free-function overloads for shared_ptr (deprecated in C++20 but still
/// provided by libc++). Both give acquire/release publication of immutable
/// snapshots — the RCU read path stays a single atomic load either way.
/// (Neither form is formally lock-free for shared_ptr; all stdlibs use a
/// lock pool for the control-block swap — identical to the prior code.)
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(_LIBCPP_VERSION)

template <typename T>
class AtomicSharedPtr {
  public:
    AtomicSharedPtr() = default;

    std::shared_ptr<T> load(
        std::memory_order order = std::memory_order_seq_cst) const {
        return ptr_.load(order);
    }
    void store(std::shared_ptr<T> desired,
               std::memory_order order = std::memory_order_seq_cst) {
        ptr_.store(std::move(desired), order);
    }
    bool compare_exchange_weak(std::shared_ptr<T>& expected,
                               std::shared_ptr<T> desired,
                               std::memory_order success,
                               std::memory_order failure) {
        return ptr_.compare_exchange_weak(expected, std::move(desired),
                                          success, failure);
    }

  private:
    std::atomic<std::shared_ptr<T>> ptr_;
};

#else  // libc++ (and any stdlib lacking atomic<shared_ptr>) — free-function fallback

template <typename T>
class AtomicSharedPtr {
  public:
    AtomicSharedPtr() = default;

    std::shared_ptr<T> load(
        std::memory_order order = std::memory_order_seq_cst) const {
        LCI_DIAG_PUSH_IGNORE_DEPRECATED
        return std::atomic_load_explicit(&ptr_, order);
        LCI_DIAG_POP
    }
    void store(std::shared_ptr<T> desired,
               std::memory_order order = std::memory_order_seq_cst) {
        LCI_DIAG_PUSH_IGNORE_DEPRECATED
        std::atomic_store_explicit(&ptr_, std::move(desired), order);
        LCI_DIAG_POP
    }
    bool compare_exchange_weak(std::shared_ptr<T>& expected,
                               std::shared_ptr<T> desired,
                               std::memory_order success,
                               std::memory_order failure) {
        LCI_DIAG_PUSH_IGNORE_DEPRECATED
        return std::atomic_compare_exchange_weak_explicit(
            &ptr_, &expected, std::move(desired), success, failure);
        LCI_DIAG_POP
    }

  private:
    mutable std::shared_ptr<T> ptr_;
};

#endif

}  // namespace lci
