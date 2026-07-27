#pragma once

#include <atomic>

// Lock-free metering taps (brief §3.3, closes issue #13 and unblocks the M3
// GUI).
//
// The audio thread stores; the UI timer loads. Nothing else. There is no FIFO,
// no queue and no allocation, because none is needed: a meter is a
// most-recent-value display, and a UI polling at 30 Hz has no use for the
// samples it would have missed. Block-rate decimation - each slot holding the
// block's peak or a one-pole-smoothed level - is both sufficient and free.
//
// Every slot is a plain float in the unit the name says, so a reader needs no
// knowledge of the DSP that produced it. Levels are linear gain (not dB) so
// the audio thread never pays for a log; the UI converts.
namespace cryp
{
    struct MeterTaps
    {
        // A meter that is not lock-free would mean the audio thread could
        // block on a UI reader, which is exactly the failure this design
        // exists to avoid. Assert it at compile time rather than trusting it.
        static_assert (std::atomic<float>::is_always_lock_free,
                        "MeterTaps requires lock-free atomic<float>: the audio thread must never block on the UI");

        // Peak magnitude of the current block, pre- and post-processing.
        std::atomic<float> inputPeakLeft { 0.0f };
        std::atomic<float> inputPeakRight { 0.0f };
        std::atomic<float> outputPeakLeft { 0.0f };
        std::atomic<float> outputPeakRight { 0.0f };

        // Per-band level, smoothed with a ~300 ms one-pole so the display sits
        // still instead of flickering.
        std::atomic<float> lowBandLevel { 0.0f };
        std::atomic<float> midBandLevel { 0.0f };
        std::atomic<float> highBandLevel { 0.0f };

        // Gain reduction, in POSITIVE decibels (0 = no reduction), which is
        // the direction a GR meter draws.
        std::atomic<float> lowCompGainReductionDb { 0.0f };
        std::atomic<float> gateGainReductionDb { 0.0f };

        void reset() noexcept
        {
            inputPeakLeft.store (0.0f, std::memory_order_relaxed);
            inputPeakRight.store (0.0f, std::memory_order_relaxed);
            outputPeakLeft.store (0.0f, std::memory_order_relaxed);
            outputPeakRight.store (0.0f, std::memory_order_relaxed);
            lowBandLevel.store (0.0f, std::memory_order_relaxed);
            midBandLevel.store (0.0f, std::memory_order_relaxed);
            highBandLevel.store (0.0f, std::memory_order_relaxed);
            lowCompGainReductionDb.store (0.0f, std::memory_order_relaxed);
            gateGainReductionDb.store (0.0f, std::memory_order_relaxed);
        }
    };
}
