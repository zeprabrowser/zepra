// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file code_space_manager.cpp
 * @brief JIT code memory management with W^X enforcement
 *
 * Manages memory for JIT-compiled machine code:
 *
 * 1. CodePage: aligned memory region for executable code
 *    - Allocated via mmap with PROT_READ|PROT_WRITE initially
 *    - Switched to PROT_READ|PROT_EXEC before execution (W^X)
 *    - Never writable and executable at the same time
 *
 * 2. CodePageAllocator: bump allocator within a code page
 *    - Objects are contiguous within a page
 *    - No per-object free; entire page is freed at once
 *
 * 3. CodeSpaceManager: manages all code pages
 *    - Tracks live/dead code pages
 *    - Handles code GC (sweep dead code, release pages)
 *    - Coordinates with the heap compactor for code relocation
 *
 * W^X (write XOR execute):
 *    On modern Linux, SELinux/hardened kernels require that a page
 *    is never writable and executable simultaneously. This manager
 *    handles the transition: write code → flip to RX → execute.
 *    For patching (IC updates, deopt), briefly flip to RW.
 *
 * Code relocation:
 *    JIT code contains PC-relative branches and call instructions.
 *    When code is relocated (defrag), all relative offsets and
 *    embedded absolute addresses must be patched.
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>
#include <unordered_map>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Code Page
// =============================================================================

static constexpr size_t CODE_PAGE_SIZE = 256 * 1024;  // 256KB
static constexpr size_t CODE_ALIGNMENT = 16;           // Code alignment

/**
 * @brief A single page of JIT code memory
 */
class CodePage {
public:
    enum class Protection : uint8_t {
        ReadWrite,      // Writing code
        ReadExecute,    // Running code
        None,           // Unmapped / released
    };

    CodePage(uintptr_t base, size_t size)
        : base_(base), size_(size), cursor_(0)
        , protection_(Protection::None)
        , liveBytes_(0), deadBytes_(0) {}

    /**
     * @brief Allocate space in this page
     * @return Offset from base, or SIZE_MAX on failure
     */
    size_t allocate(size_t codeSize) {
        codeSize = alignUp(codeSize, CODE_ALIGNMENT);
        if (cursor_ + codeSize > size_) return SIZE_MAX;

        size_t offset = cursor_;
        cursor_ += codeSize;
        liveBytes_ += codeSize;
        return offset;
    }

    /**
     * @brief Mark bytes as dead (for code GC accounting)
     */
    void markDead(size_t offset, size_t size) {
        (void)offset;
        deadBytes_ += size;
        if (liveBytes_ >= size) liveBytes_ -= size;
    }

    /**
     * @brief Switch protection to read-write (for code emission)
     */
    bool makeWritable() {
#ifdef __linux__
        int result = mprotect(reinterpret_cast<void*>(base_), size_,
                              PROT_READ | PROT_WRITE);
        if (result == 0) {
            protection_ = Protection::ReadWrite;
            return true;
        }
        return false;
#else
        protection_ = Protection::ReadWrite;
        return true;
#endif
    }

    /**
     * @brief Switch protection to read-execute (for running code)
     */
    bool makeExecutable() {
#ifdef __linux__
        int result = mprotect(reinterpret_cast<void*>(base_), size_,
                              PROT_READ | PROT_EXEC);
        if (result == 0) {
            protection_ = Protection::ReadExecute;
            return true;
        }
        return false;
#else
        protection_ = Protection::ReadExecute;
        return true;
#endif
    }

    /**
     * @brief Write code into the page
     *
     * Temporarily makes page writable, copies code, then restores.
     */
    bool writeCode(size_t offset, const void* code, size_t size) {
        if (offset + size > size_) return false;

        bool wasExecutable = (protection_ == Protection::ReadExecute);
        if (wasExecutable) {
            if (!makeWritable()) return false;
        }

        std::memcpy(reinterpret_cast<void*>(base_ + offset), code, size);

        if (wasExecutable) {
            if (!makeExecutable()) return false;
        }

        return true;
    }

    /**
     * @brief Patch a single pointer in code (e.g. for IC update)
     */
    bool patchPointer(size_t offset, uintptr_t newValue) {
        return writeCode(offset, &newValue, sizeof(uintptr_t));
    }

    // Accessors
    uintptr_t base() const { return base_; }
    size_t size() const { return size_; }
    size_t used() const { return cursor_; }
    size_t remaining() const { return size_ - cursor_; }
    size_t liveBytes() const { return liveBytes_; }
    size_t deadBytes() const { return deadBytes_; }
    Protection protection() const { return protection_; }
    bool isEmpty() const { return liveBytes_ == 0; }

    double occupancy() const {
        return cursor_ > 0
            ? static_cast<double>(liveBytes_) / static_cast<double>(cursor_)
            : 0;
    }

    bool containsAddress(uintptr_t addr) const {
        return addr >= base_ && addr < base_ + size_;
    }

private:
    static size_t alignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    uintptr_t base_;
    size_t size_;
    size_t cursor_;
    Protection protection_;
    size_t liveBytes_;
    size_t deadBytes_;
};

// =============================================================================
// Code Space Manager
// =============================================================================

class CodeSpaceManager {
public:
    struct Config {
        size_t pageSize;
        size_t maxPages;
        double compactionThreshold;  // Compact when occupancy below this

        Config()
            : pageSize(CODE_PAGE_SIZE)
            , maxPages(256)
            , compactionThreshold(0.50) {}
    };

    struct Stats {
        size_t totalPages;
        size_t totalBytes;
        size_t usedBytes;
        size_t liveBytes;
        size_t deadBytes;
        size_t pagesReleased;
        size_t allocations;
        size_t allocationFailures;
    };

    explicit CodeSpaceManager(const Config& config = Config{})
        : config_(config) {}

    ~CodeSpaceManager() { releaseAll(); }

    CodeSpaceManager(const CodeSpaceManager&) = delete;
    CodeSpaceManager& operator=(const CodeSpaceManager&) = delete;

    /**
     * @brief Allocate code space
     * @return Pair of (base address, page index) or (0, -1) on failure
     */
    std::pair<uintptr_t, uint32_t> allocate(size_t codeSize) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.allocations++;

        // Try current page
        if (!pages_.empty()) {
            auto& current = pages_.back();
            size_t offset = current->allocate(codeSize);
            if (offset != SIZE_MAX) {
                return {current->base() + offset,
                        static_cast<uint32_t>(pages_.size() - 1)};
            }
        }

        // Allocate new page
        if (pages_.size() >= config_.maxPages) {
            stats_.allocationFailures++;
            return {0, UINT32_MAX};
        }

        auto page = allocatePage();
        if (!page) {
            stats_.allocationFailures++;
            return {0, UINT32_MAX};
        }

        page->makeWritable();
        size_t offset = page->allocate(codeSize);
        uintptr_t base = page->base();

        uint32_t pageIdx = static_cast<uint32_t>(pages_.size());
        pages_.push_back(std::move(page));

        return {base + offset, pageIdx};
    }

    /**
     * @brief Write code to allocated space
     */
    bool emitCode(uintptr_t addr, const void* code, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* page = findPageContaining(addr);
        if (!page) return false;

        size_t offset = addr - page->base();
        return page->writeCode(offset, code, size);
    }

    /**
     * @brief Finalize code (make executable, flush icache)
     */
    bool finalizeCode(uintptr_t addr) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* page = findPageContaining(addr);
        if (!page) return false;

        bool result = page->makeExecutable();

#ifdef __linux__
        // Flush instruction cache
        __builtin___clear_cache(
            reinterpret_cast<char*>(page->base()),
            reinterpret_cast<char*>(page->base() + page->used()));
#endif

        return result;
    }

    /**
     * @brief Patch a pointer in code (for IC updates, deopt)
     */
    bool patchCode(uintptr_t addr, uintptr_t newValue) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* page = findPageContaining(addr);
        if (!page) return false;

        size_t offset = addr - page->base();
        bool result = page->patchPointer(offset, newValue);

#ifdef __linux__
        // Flush icache around patched location
        __builtin___clear_cache(
            reinterpret_cast<char*>(addr),
            reinterpret_cast<char*>(addr + sizeof(uintptr_t)));
#endif

        return result;
    }

    /**
     * @brief Mark code as dead (for GC)
     */
    void markCodeDead(uintptr_t addr, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* page = findPageContaining(addr);
        if (!page) return;

        size_t offset = addr - page->base();
        page->markDead(offset, size);
    }

    /**
     * @brief Release empty pages back to OS
     */
    size_t releaseEmptyPages() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t released = 0;

        auto it = pages_.begin();
        while (it != pages_.end()) {
            if ((*it)->isEmpty()) {
                releasePage(it->get());
                it = pages_.erase(it);
                released++;
                stats_.pagesReleased++;
            } else {
                ++it;
            }
        }

        return released;
    }

    /**
     * @brief Check if address is in code space
     */
    bool containsAddress(uintptr_t addr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return findPageContaining(addr) != nullptr;
    }

    Stats computeStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats stats = stats_;
        stats.totalPages = pages_.size();

        for (const auto& page : pages_) {
            stats.totalBytes += page->size();
            stats.usedBytes += page->used();
            stats.liveBytes += page->liveBytes();
            stats.deadBytes += page->deadBytes();
        }

        return stats;
    }

private:
    std::unique_ptr<CodePage> allocatePage() {
#ifdef __linux__
        void* mem = mmap(nullptr, config_.pageSize,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;

        return std::make_unique<CodePage>(
            reinterpret_cast<uintptr_t>(mem), config_.pageSize);
#else
        void* mem = zepra_aligned_alloc(4096, config_.pageSize);
        if (!mem) return nullptr;
        return std::make_unique<CodePage>(
            reinterpret_cast<uintptr_t>(mem), config_.pageSize);
#endif
    }

    void releasePage(CodePage* page) {
#ifdef __linux__
        munmap(reinterpret_cast<void*>(page->base()), page->size());
#else
        std::free(reinterpret_cast<void*>(page->base()));
#endif
    }

    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& page : pages_) {
            releasePage(page.get());
        }
        pages_.clear();
    }

    CodePage* findPageContaining(uintptr_t addr) const {
        for (const auto& page : pages_) {
            if (page->containsAddress(addr)) return page.get();
        }
        return nullptr;
    }

    Config config_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<CodePage>> pages_;
    Stats stats_{};
};

} // namespace Zepra::Heap
