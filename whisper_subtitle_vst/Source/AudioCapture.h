#pragma once

#include "Resampler.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <functional>
#include <vector>

/**
    Records what plays through the plugin, aligned to the host timeline.

    The audio thread only downmixes into lock-free FIFOs. A background thread
    drains them, low-pass filters and resamples to 16 kHz mono (what Whisper
    expects) and groups contiguous playback into "regions". A region ends when
    playback stops, the playhead jumps, or capture is disarmed.
*/
class AudioCapture : private juce::Thread
{
public:
    static constexpr double whisperSampleRate = 16000.0;

    struct Region
    {
        int id = 0;
        double startSeconds = 0.0;     // host timeline position of the first sample
        std::vector<float> samples;    // 16 kHz mono

        double lengthSeconds() const   { return (double) samples.size() / whisperSampleRate; }
        double endSeconds() const      { return startSeconds + lengthSeconds(); }
    };

    AudioCapture();
    ~AudioCapture() override;

    void prepare (double hostSampleRate, int maximumBlockSize);

    /** Stops the capture thread; onRegionFinished won't fire afterwards. */
    void shutdown()                         { stopThread (2000); }

    /** Audio thread. timelineSample is the host position of buffer sample 0. */
    void push (const juce::AudioBuffer<float>& buffer, juce::int64 timelineSample,
               bool isPlaying, bool isNonRealtime);

    std::atomic<bool> armed { true };

    /** Called on the capture thread whenever a region is complete (and long enough to keep). */
    std::function<void (const Region&)> onRegionFinished;

    /** Copies of all regions, including the one currently recording. */
    std::vector<Region> getRegionsSnapshot() const;

    double getCapturedSeconds() const;
    bool isRecording() const                { return recording.load(); }
    int getDroppedBlocks() const            { return droppedBlocks.load(); }
    void clear();

private:
    struct BlockInfo
    {
        juce::int64 timelineSample = 0;
        int numSamples = 0;
        bool stopMarker = false;
    };

    void run() override;
    void handleBlock (const BlockInfo&, float* data);
    void finishCurrentRegion();

    // --- audio thread -> capture thread
    std::vector<float> sampleStorage;
    std::vector<BlockInfo> infoStorage;
    juce::AbstractFifo sampleFifo { 1 }, infoFifo { 1 };
    bool wasCapturing = false;                 // audio thread only
    std::atomic<double> hostRate { 48000.0 };
    std::atomic<int> droppedBlocks { 0 };
    std::atomic<bool> recording { false };

    // --- capture thread
    std::vector<float> scratch;
    WhisperResampler resampler;
    juce::int64 expectedNextSample = -1;

    // --- shared
    mutable juce::CriticalSection regionLock;
    std::vector<Region> regions;
    Region current;
    bool hasCurrent = false;
    int nextRegionId = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioCapture)
};
