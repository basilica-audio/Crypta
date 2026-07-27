#include "NoiseGateStage.h"

namespace cryp
{
    void NoiseGateStage::prepare (const juce::dsp::ProcessSpec& spec)
    {
        gate.prepare (spec);
        modernGate.prepare (spec);
    }

    void NoiseGateStage::reset()
    {
        gate.reset();
        modernGate.reset();
    }
}
