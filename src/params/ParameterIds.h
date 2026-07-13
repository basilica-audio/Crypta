#pragma once

// Central definition of all AudioProcessorValueTreeState parameter IDs.
// Keeping these in one place avoids typo-mismatches between the layout
// creation code, the processor's parameter lookups, and any future GUI code.

namespace ParamIDs
{
    inline constexpr auto inputGain = "inputGain";
    inline constexpr auto outputGain = "outputGain";
    inline constexpr auto bypass = "bypass";
}
