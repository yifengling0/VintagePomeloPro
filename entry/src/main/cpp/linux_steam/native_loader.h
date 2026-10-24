#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace steamohos {
// Fixed native-ISA rtld bootstrap for the isolated diagnostic process. The
// original file supplies RX pages; GNU rtld supplies all relocations and TLS.
// No guest code runs in Prepare(), and Enter() never returns to the host libc.
class NativeLoaderImage {
public:
    ~NativeLoaderImage();
    NativeLoaderImage() = default;
    NativeLoaderImage(const NativeLoaderImage&) = delete;
    NativeLoaderImage& operator=(const NativeLoaderImage&) = delete;
    bool Prepare(const std::vector<std::string>& argv, const std::vector<std::string>& environment);
    // Single-thread GNU exec only: reclaim the previous address space from a
    // separate raw-syscall trampoline after switching to the new loader stack.
    // Preparation does not alter any existing mapping; failure is reversible.
    bool PrepareExecCleanup();
    [[noreturn]] void Enter() const;
    std::string stage = "not_prepared";
    int error = 0;
    std::string diagnostics;
private:
    void* image_ = nullptr;
    size_t imageBytes_ = 0;
    void* stack_ = nullptr;
    size_t stackBytes_ = 0;
    uintptr_t entry_ = 0, sp_ = 0;
    void* cleanup_ = nullptr;
    size_t cleanupBytes_ = 0, cleanupCount_ = 0;
    bool Fail(const char* value, int code);
};
}
