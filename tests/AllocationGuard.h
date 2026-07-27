#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

// Global allocation counter, for asserting that the audio thread never
// allocates (brief §6 T16).
//
// Real-time safety is the one property of this plugin that cannot be measured
// from the audio it produces: a processBlock() that calls malloc sounds
// perfect right up until the moment the allocator takes a lock and the buffer
// underruns. So it has to be instrumented directly.
//
// The mechanism is a replacement of the global operator new/delete. That is a
// heavy hammer, but it is the only one that catches allocations from anywhere
// - including inside JUCE, inside the standard library, and inside code that
// was not written with this test in mind, which is exactly where a real
// regression would hide.
//
// The counter is atomic and process-wide. Tests using it must not run
// concurrently with other allocating work, which Catch2's default
// single-threaded runner guarantees.
namespace AllocationCounter
{
    inline std::atomic<long long>& counter() noexcept
    {
        static std::atomic<long long> count { 0 };
        return count;
    }

    inline long long current() noexcept { return counter().load (std::memory_order_relaxed); }

    // Counts allocations that happen while `callable` runs.
    template <typename Callable>
    long long countDuring (Callable&& callable)
    {
        const auto before = current();
        callable();
        return current() - before;
    }
}

// The replacements themselves. Defined inline in a header included by exactly
// one translation unit (RobustnessTests.cpp) to keep them from being emitted
// twice - the ODR rules for replaceable global allocation functions are
// unforgiving, and two definitions is undefined behaviour rather than a link
// error on some toolchains.
#if defined (CRYPTA_DEFINE_ALLOCATION_COUNTER)

void* operator new (std::size_t size)
{
    AllocationCounter::counter().fetch_add (1, std::memory_order_relaxed);

    if (size == 0)
        size = 1;

    if (auto* pointer = std::malloc (size))
        return pointer;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return operator new (size);
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    AllocationCounter::counter().fetch_add (1, std::memory_order_relaxed);
    return std::malloc (size == 0 ? 1 : size);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    AllocationCounter::counter().fetch_add (1, std::memory_order_relaxed);
    return std::malloc (size == 0 ? 1 : size);
}

void operator delete (void* pointer) noexcept { std::free (pointer); }
void operator delete[] (void* pointer) noexcept { std::free (pointer); }
void operator delete (void* pointer, std::size_t) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept { std::free (pointer); }
void operator delete (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }

// Aligned forms (C++17). JUCE's SIMD types are over-aligned, so these are not
// hypothetical - missing them would route some allocations around the counter
// and quietly weaken the test.
void* operator new (std::size_t size, std::align_val_t alignment)
{
    AllocationCounter::counter().fetch_add (1, std::memory_order_relaxed);

    const auto alignmentValue = static_cast<std::size_t> (alignment);

    // aligned_alloc requires the size to be a multiple of the alignment.
    const auto roundedSize = ((size == 0 ? 1 : size) + alignmentValue - 1) / alignmentValue * alignmentValue;

    if (auto* pointer = std::aligned_alloc (alignmentValue, roundedSize))
        return pointer;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return operator new (size, alignment);
}

void operator delete (void* pointer, std::align_val_t) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, std::align_val_t) noexcept { std::free (pointer); }
void operator delete (void* pointer, std::size_t, std::align_val_t) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, std::size_t, std::align_val_t) noexcept { std::free (pointer); }

#endif
