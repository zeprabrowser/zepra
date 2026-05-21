// zepra_alloc.h - Portable aligned allocation for Zepra
// std::aligned_alloc is not available on Windows/MinGW
#pragma once
#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#include <malloc.h>
inline void* zepra_aligned_alloc(size_t alignment, size_t size) {
    return _aligned_malloc(size, alignment);
}
inline void zepra_aligned_free(void* ptr) {
    _aligned_free(ptr);
}
#else
inline void* zepra_aligned_alloc(size_t alignment, size_t size) {
    return zepra_aligned_alloc(alignment, size);
}
inline void zepra_aligned_free(void* ptr) {
    std::free(ptr);
}
#endif