#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstddef>
#include <vector>

// First-order antiderivative antialiasing (ADAA-1) for memoryless
// nonlinearities - Parker, Zavalishin & Le Bivic, "Reducing the aliasing of
// nonlinear waveshaping using continuous-time convolution", DAFx-16 (see
// .scaffold/research/2026-07-25-sota/research-triode-adaa.md §2.4).
//
// For a nonlinearity f with first antiderivative F1:
//
//   y[n] = (F1(x[n]) - F1(x[n-1])) / (x[n] - x[n-1])         if |dx| > eps
//   y[n] = f((x[n] + x[n-1]) / 2)                            otherwise
//
// The quotient is the average of f over the segment between consecutive input
// samples, i.e. exactly the continuous-time convolution of f(x(t)) with a
// one-sample box - which is what suppresses the alias products a naive
// per-sample waveshaper folds back into the audio band. Measured suppression
// on tanh/hard-clip curves is ~20-30 dB, which is why this release can drop
// from two 4x oversampling regions to one shared region and still come out
// well ahead on aliasing.
//
// Two costs are inherent and deliberately accepted here:
//   - a half-sample group delay. It is identical for every ADAA-1 core, so
//     the Mid and High bands stay aligned with each other, and it lives
//     inside the oversampled region where it is a quarter- or eighth-sample
//     at base rate - far below the integer granularity of setLatencySamples()
//     and therefore not reported.
//   - a mild sinc-like HF droop, cos(pi*f/fs) at the oversampled rate. At 4x
//     48 kHz that is about -0.23 dB at 14 kHz, which is why this is used ON
//     TOP of oversampling rather than instead of it (the research file is
//     explicit about that), and why the transparency contract in the tests is
//     stated as +/-0.5 dB rather than +/-0.1 dB.
//
// The state is double precision even though the audio is float: the quotient
// is a difference-over-difference and loses precision exactly where the
// fallback branch has not yet taken over.
namespace cryp
{
    //==========================================================================
    // Closed-form curves. These need no table: their antiderivatives are
    // elementary, so they are both cheaper and exact.

    // f(x) = tanh(x), F1(x) = ln(cosh(x)).
    struct TanhCurve
    {
        static double f (double x) noexcept { return std::tanh (x); }

        static double antiderivative (double x) noexcept
        {
            // ln(cosh(x)) overflows for |x| > ~710 if evaluated literally.
            // |x| + log1p(exp(-2|x|)) - ln(2) is the same function, evaluated
            // without ever forming cosh - and log1p keeps full precision in
            // the small-x limit where the naive form would cancel.
            constexpr double lnTwo = 0.6931471805599453;
            const auto absX = std::abs (x);
            return absX + std::log1p (std::exp (-2.0 * absX)) - lnTwo;
        }
    };

    // f(x) = clamp(x, -1, 1), F1(x) = x^2/2 inside the linear region,
    // |x| - 1/2 outside (the two pieces agree in value and slope at |x| = 1).
    struct HardClipCurve
    {
        static double f (double x) noexcept { return juce::jlimit (-1.0, 1.0, x); }

        static double antiderivative (double x) noexcept
        {
            const auto absX = std::abs (x);
            return absX <= 1.0 ? 0.5 * x * x : absX - 0.5;
        }
    };

    //==========================================================================
    // Tabulated curve, for nonlinearities whose antiderivative has no useful
    // closed form - the Yeh tanh-fit x/(1+|x|^2.5)^(1/2.5) and the DC solution
    // of the asymmetric shunt diode clipper, both of which this plugin needs
    // (research-diode-clipper-dk.md §3.1: "1024-4096 points, cubic").
    //
    // Both f and F1 are tabulated on the same uniform grid; F1 is obtained by
    // integrating the sampled f with Simpson's rule on a finer sub-grid, so
    // the tabulated pair stays mutually consistent (an F1 that is not really
    // the antiderivative of the f being used produces a DC offset, not just
    // an approximation error).
    //
    // build() allocates and is prepare()-time only. evaluate()/antiderivative()
    // are allocation-free and real-time safe.
    class ShaperTable
    {
    public:
        static constexpr int defaultNumPoints = 2048;

        // Tabulates `curve` over [-inputRange, +inputRange]. `curve` is only
        // called during this build, never on the audio thread, so it may be
        // arbitrarily expensive (e.g. a Newton solve per point).
        template <typename CurveFunction>
        void build (CurveFunction&& curve, double inputRange, int numPoints = defaultNumPoints)
        {
            jassert (inputRange > 0.0);
            jassert (numPoints >= 16);

            range = inputRange;
            values.resize (static_cast<size_t> (numPoints));
            integrals.resize (static_cast<size_t> (numPoints));
            step = 2.0 * range / static_cast<double> (numPoints - 1);
            inverseStep = 1.0 / step;

            for (int index = 0; index < numPoints; ++index)
                values[static_cast<size_t> (index)] = curve (-range + step * static_cast<double> (index));

            // Simpson's rule across each cell, sampling `curve` at the cell
            // midpoint as well, then a running sum. Anchoring the running sum
            // at F1(-range) = 0 is arbitrary but harmless: ADAA only ever uses
            // *differences* of F1, so any constant of integration cancels.
            integrals[0] = 0.0;

            for (int index = 1; index < numPoints; ++index)
            {
                const auto left = -range + step * static_cast<double> (index - 1);
                const auto right = left + step;
                const auto middle = curve (0.5 * (left + right));

                const auto cell = (step / 6.0)
                                   * (values[static_cast<size_t> (index - 1)] + 4.0 * middle + values[static_cast<size_t> (index)]);

                integrals[static_cast<size_t> (index)] = integrals[static_cast<size_t> (index - 1)] + cell;
            }

            edgeValueLow = values.front();
            edgeValueHigh = values.back();
            edgeIntegralLow = integrals.front();
            edgeIntegralHigh = integrals.back();
        }

        bool isBuilt() const noexcept { return ! values.empty(); }

        // Spelled f() as well as evaluate() so a ShaperTable satisfies the
        // same duck-typed interface as the closed-form curves and can be
        // handed to ADAAState::process() interchangeably.
        double f (double x) const noexcept { return evaluate (x); }

        double evaluate (double x) const noexcept
        {
            // Outside the table the curve is treated as fully saturated, which
            // is exact for every curve tabulated here (all of them flatten
            // long before the table edge - the range is chosen for that).
            if (x <= -range)
                return edgeValueLow;

            if (x >= range)
                return edgeValueHigh;

            return interpolate (values, x);
        }

        double antiderivative (double x) const noexcept
        {
            // Consistent with evaluate()'s saturation: if f is constant beyond
            // the edge then F1 continues linearly with that constant slope.
            // Getting this right matters more than it looks - ADAA divides by
            // (x[n] - x[n-1]), so an F1 that does not match the f actually
            // being applied shows up as a spurious DC step on overload.
            if (x <= -range)
                return edgeIntegralLow + edgeValueLow * (x + range);

            if (x >= range)
                return edgeIntegralHigh + edgeValueHigh * (x - range);

            return interpolate (integrals, x);
        }

    private:
        // Catmull-Rom cubic on the uniform grid, clamping the stencil at the
        // table edges (the neighbours are flat there anyway).
        double interpolate (const std::vector<double>& table, double x) const noexcept
        {
            const auto position = (x + range) * inverseStep;
            const auto lower = static_cast<int> (std::floor (position));
            const auto fraction = position - static_cast<double> (lower);

            const auto lastIndex = static_cast<int> (table.size()) - 1;
            const auto at = [&table, lastIndex] (int index) noexcept
            {
                return table[static_cast<size_t> (juce::jlimit (0, lastIndex, index))];
            };

            const auto p0 = at (lower - 1);
            const auto p1 = at (lower);
            const auto p2 = at (lower + 1);
            const auto p3 = at (lower + 2);

            const auto a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
            const auto b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            const auto c = -0.5 * p0 + 0.5 * p2;

            return ((a * fraction + b) * fraction + c) * fraction + p1;
        }

        std::vector<double> values;
        std::vector<double> integrals;
        double range = 1.0;
        double step = 1.0;
        double inverseStep = 1.0;
        double edgeValueLow = 0.0;
        double edgeValueHigh = 0.0;
        double edgeIntegralLow = 0.0;
        double edgeIntegralHigh = 0.0;
    };

    //==========================================================================
    // Per-channel ADAA-1 state. One instance per channel per nonlinear stage;
    // process() is the hot path.
    class ADAAState
    {
    public:
        void reset() noexcept { previousInput = 0.0; }

        // Seeds the one-sample input history with a known operating point, so
        // the first process() call after a reset sees no spurious input step.
        // Without this, a stage whose quiescent input is a non-zero constant
        // (the Circuit high band's bias offset) starts every fresh render with
        // one sample of the ADAA quotient averaged over [0, operating point] -
        // half the offset, as an impulse into everything downstream. Called
        // from reset()-time code only, never mid-stream.
        void prime (double quiescentInput) noexcept { previousInput = quiescentInput; }

        // `curve` must expose f(x) and antiderivative(x). Both the closed-form
        // structs above (as static members) and ShaperTable (as instance
        // members) satisfy this, so the call sites read the same either way.
        template <typename Curve>
        double process (double x, const Curve& curve) noexcept
        {
            const auto previous = previousInput;
            previousInput = x;

            const auto delta = x - previous;

            // The ill-conditioned case: as delta -> 0 the quotient becomes
            // 0/0. The threshold has to scale with the signal, because the
            // absolute precision of the F1 difference does - a fixed epsilon
            // would leave the quotient noisy on loud material and pointlessly
            // desensitised on quiet material.
            const auto epsilon = 1.0e-6 * juce::jmax (1.0, std::abs (x));

            if (std::abs (delta) > epsilon)
                return (curve.antiderivative (x) - curve.antiderivative (previous)) / delta;

            // Midpoint fallback: the limit of the quotient, and second-order
            // accurate rather than merely continuous with it.
            return curve.f (0.5 * (x + previous));
        }

    private:
        double previousInput = 0.0;
    };

    // Adapter that lets the closed-form curves (whose f/antiderivative are
    // static) be passed to ADAAState::process() by value, keeping one call
    // shape for both closed-form and tabulated curves.
    template <typename ClosedFormCurve>
    struct ClosedFormShaper
    {
        static double f (double x) noexcept { return ClosedFormCurve::f (x); }
        static double antiderivative (double x) noexcept { return ClosedFormCurve::antiderivative (x); }
    };

    using TanhShaper = ClosedFormShaper<TanhCurve>;
    using HardClipShaper = ClosedFormShaper<HardClipCurve>;
}
