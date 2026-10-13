#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

/** Streams mono audio at any rate into 16 kHz (what Whisper expects):
    low-pass to remove what can't be represented, then linear interpolation. */
class WhisperResampler
{
public:
    static constexpr double outputRate = 16000.0;

    void prepare (double inputRate)
    {
        rate = inputRate;
        filter.setCoefficients (juce::IIRCoefficients::makeLowPass (inputRate, juce::jmin (7000.0, inputRate * 0.45)));
        reset();
    }

    void reset()
    {
        filter.reset();
        phase = 0.0;
        previous = 0.0f;
    }

    /** Filters 'data' in place and appends the resampled result to 'out'. */
    void process (float* data, int numSamples, std::vector<float>& out)
    {
        if (numSamples <= 0)
            return;

        filter.processSamples (data, numSamples);

        const double step = rate / outputRate;

        // virtual input: y[0] = previous, y[k + 1] = data[k]
        while (phase < (double) numSamples)
        {
            const int i = (int) phase;
            const float frac = (float) (phase - (double) i);
            const float a = i == 0 ? previous : data[i - 1];
            out.push_back (a + (data[i] - a) * frac);
            phase += step;
        }

        phase -= (double) numSamples;
        previous = data[numSamples - 1];
    }

private:
    juce::IIRFilter filter;
    double rate = 48000.0, phase = 0.0;
    float previous = 0.0f;
};
