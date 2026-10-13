#include "AudioCapture.h"

#include <cmath>

namespace
{
    constexpr double fifoSeconds = 20.0;              // headroom if the capture thread stalls
    constexpr double minimumRegionSeconds = 0.75;     // ignore scrubbing / tiny blips
    constexpr juce::int64 jumpToleranceSamples = 64;  // hosts sometimes wobble by a few samples
}

AudioCapture::AudioCapture() : juce::Thread ("Whisper capture")
{
    prepare (48000.0, 512);
}

AudioCapture::~AudioCapture()
{
    stopThread (2000);
}

void AudioCapture::prepare (double hostSampleRate, int maximumBlockSize)
{
    stopThread (2000);

    {
        const juce::ScopedLock sl (regionLock);
        finishCurrentRegion();
    }

    hostRate = hostSampleRate;

    auto fifoSize = juce::jmax ((int) (hostSampleRate * fifoSeconds), maximumBlockSize * 4);
    sampleStorage.assign ((size_t) fifoSize, 0.0f);
    sampleFifo.setTotalSize (fifoSize);
    sampleFifo.reset();

    infoStorage.assign (8192, {});
    infoFifo.setTotalSize ((int) infoStorage.size());
    infoFifo.reset();

    scratch.assign ((size_t) fifoSize, 0.0f);
    resampler.prepare (hostSampleRate);
    wasCapturing = false;
    expectedNextSample = -1;

    startThread (juce::Thread::Priority::low);
}

//==============================================================================
void AudioCapture::push (const juce::AudioBuffer<float>& buffer, juce::int64 timelineSample,
                         bool isPlaying, bool isNonRealtime)
{
    const bool capturing = isPlaying && armed.load (std::memory_order_relaxed);

    auto writeInfo = [this, isNonRealtime] (const BlockInfo& info) -> bool
    {
        while (infoFifo.getFreeSpace() < 1)
        {
            if (! isNonRealtime)
                return false;

            juce::Thread::sleep (1); // offline bounce: safe to wait for the capture thread
        }

        const auto scope = infoFifo.write (1);
        if (scope.blockSize1 > 0) infoStorage[(size_t) scope.startIndex1] = info;
        else                      infoStorage[(size_t) scope.startIndex2] = info;
        return true;
    };

    if (! capturing)
    {
        if (wasCapturing)
            wasCapturing = ! writeInfo ({ 0, 0, true });

        return;
    }

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
        return;

    while (sampleFifo.getFreeSpace() < numSamples || infoFifo.getFreeSpace() < 1)
    {
        if (! isNonRealtime)
        {
            // Drop the block; the gap shows up as a timeline jump and starts a new region.
            droppedBlocks.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        juce::Thread::sleep (1);
    }

    const float gain = 1.0f / (float) numChannels;

    {
        const auto scope = sampleFifo.write (numSamples);

        auto mix = [&] (int destStart, int count, int srcOffset)
        {
            if (count <= 0)
                return;

            auto* dest = sampleStorage.data() + destStart;
            juce::FloatVectorOperations::copyWithMultiply (dest, buffer.getReadPointer (0, srcOffset), gain, count);

            for (int ch = 1; ch < numChannels; ++ch)
                juce::FloatVectorOperations::addWithMultiply (dest, buffer.getReadPointer (ch, srcOffset), gain, count);
        };

        mix (scope.startIndex1, scope.blockSize1, 0);
        mix (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    writeInfo ({ timelineSample, numSamples, false });
    wasCapturing = true;
}

//==============================================================================
void AudioCapture::run()
{
    while (! threadShouldExit())
    {
        bool didWork = false;

        while (infoFifo.getNumReady() > 0)
        {
            BlockInfo info;
            {
                const auto scope = infoFifo.read (1);
                info = scope.blockSize1 > 0 ? infoStorage[(size_t) scope.startIndex1]
                                            : infoStorage[(size_t) scope.startIndex2];
            }

            if (info.numSamples > 0)
            {
                const auto scope = sampleFifo.read (info.numSamples);

                if (scope.blockSize1 > 0)
                    std::copy_n (sampleStorage.data() + scope.startIndex1, scope.blockSize1, scratch.data());

                if (scope.blockSize2 > 0)
                    std::copy_n (sampleStorage.data() + scope.startIndex2, scope.blockSize2,
                                 scratch.data() + scope.blockSize1);
            }

            handleBlock (info, scratch.data());
            didWork = true;
        }

        if (! didWork)
            wait (20);
    }
}

void AudioCapture::handleBlock (const BlockInfo& info, float* data)
{
    const juce::ScopedLock sl (regionLock);

    if (info.stopMarker)
    {
        finishCurrentRegion();
        return;
    }

    const double rate = hostRate.load();

    const bool jumped = expectedNextSample < 0
                     || std::abs (info.timelineSample - expectedNextSample) > jumpToleranceSamples;

    if (! hasCurrent || jumped)
    {
        finishCurrentRegion();

        current = {};
        current.id = nextRegionId++;
        current.startSeconds = (double) info.timelineSample / rate;
        current.samples.reserve ((size_t) (whisperSampleRate * 60.0));
        hasCurrent = true;
        recording = true;

        resampler.reset();
    }

    expectedNextSample = info.timelineSample + info.numSamples;

    resampler.process (data, info.numSamples, current.samples);
}

void AudioCapture::finishCurrentRegion()
{
    // regionLock must be held
    if (! hasCurrent)
        return;

    hasCurrent = false;
    recording = false;
    expectedNextSample = -1;

    if (current.lengthSeconds() < minimumRegionSeconds)
        return;

    // A new pass over the same material supersedes older takes it fully covers.
    const double start = current.startSeconds, end = current.endSeconds();
    regions.erase (std::remove_if (regions.begin(), regions.end(),
                                   [&] (const Region& r) { return r.startSeconds >= start - 0.05
                                                               && r.endSeconds() <= end + 0.05; }),
                   regions.end());

    regions.push_back (std::move (current));
    current = {};

    if (onRegionFinished)
        onRegionFinished (regions.back());
}

//==============================================================================
std::vector<AudioCapture::Region> AudioCapture::getRegionsSnapshot() const
{
    const juce::ScopedLock sl (regionLock);
    auto copy = regions;

    if (hasCurrent && current.lengthSeconds() >= minimumRegionSeconds)
        copy.push_back (current);

    return copy;
}

double AudioCapture::getCapturedSeconds() const
{
    const juce::ScopedLock sl (regionLock);
    double total = hasCurrent ? current.lengthSeconds() : 0.0;

    for (auto& r : regions)
        total += r.lengthSeconds();

    return total;
}

void AudioCapture::clear()
{
    const juce::ScopedLock sl (regionLock);
    regions.clear();
    regions.shrink_to_fit();
    current = {};
    hasCurrent = false;
    recording = false;
    expectedNextSample = -1;
    droppedBlocks = 0;
}
