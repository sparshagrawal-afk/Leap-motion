#pragma once

#include "ARASupport.h"
#include "AudioCapture.h"
#include "Subtitles.h"
#include "Transcriber.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

class WhisperSubtitlesProcessor : public juce::AudioProcessor,
                                 #if JucePlugin_Enable_ARA
                                  public juce::AudioProcessorARAExtension,
                                 #endif
                                  private juce::Timer
{
public:
    WhisperSubtitlesProcessor();
    ~WhisperSubtitlesProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                 { return true; }

    const juce::String getName() const override     { return JucePlugin_Name; }
    bool acceptsMidi() const override               { return false; }
    bool producesMidi() const override              { return false; }
    bool isMidiEffect() const override              { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override                   { return 1; }
    int getCurrentProgram() override                { return 0; }
    void setCurrentProgram (int) override           {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

   #if JucePlugin_Enable_ARA
    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }
   #endif

    //==============================================================================
    AudioCapture capture;

    /** One Whisper engine shared by every instance of the plugin, so the model loads once. */
    juce::SharedResourcePointer<Transcriber> transcriber;

    /** True when the host runs this instance in ARA mode (it hands over whole clips). */
    bool isUsingARA() const;

    /** e.g. "3 clips, 2 transcribed" (ARA mode only). */
    juce::String getARAStatus() const;

    void setModelFile (const juce::File&);
    juce::File getModelFile() const;

    Transcriber::Settings getSettings() const;
    void setSettings (const Transcriber::Settings&);

    std::atomic<bool> autoTranscribe { true };

    /** Seconds subtracted from every cue on export (e.g. 3600 for a 01:00:00:00 session start). */
    std::atomic<double> exportOffsetSeconds { 0.0 };

    /** Editor shows only the subtitle line (a movable "subtitle screen"). */
    std::atomic<bool> compactView { false };
    std::atomic<int> compactWidth { 760 }, compactHeight { 150 };

    /** Transcribes every captured region, or in ARA mode every clip
        (only those without cues yet unless 'all'). */
    void transcribeCaptured (bool all);

    SubtitleList getSubtitles() const;
    void setSubtitles (SubtitleList);
    void clearSubtitles();

    /** Bumped whenever the cue list changes, so the editor knows when to refresh. */
    int getSubtitleVersion() const                  { return subtitleVersion.load(); }

    double getPlayheadSeconds() const               { return playheadSeconds.load(); }
    bool isHostPlaying() const                      { return hostPlaying.load(); }

private:
    void timerCallback() override;
    void handleRegionFinished (const AudioCapture::Region&);
    void handleTranscription (double regionStart, double regionEnd, SubtitleList cues);

    /** Wraps a transcription callback so it is skipped once this instance is gone
        (the engine is shared and may outlive us). */
    Transcriber::ResultCallback guarded (Transcriber::ResultCallback);

    struct Lifetime
    {
        std::mutex lock;
        bool alive = true;
    };

    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();

   #if JucePlugin_Enable_ARA
    SubtitleDocumentController* getARADocumentController() const;
    std::vector<juce::ARAPlaybackRegion*> getARARegions() const;
    void refreshFromARA();
    void transcribeARAClips (bool all);
    void queueARASource (juce::ARAAudioSource*, const Transcriber::Settings&);

    struct ARAResult
    {
        juce::String sourceId, settingsKey;
        bool ok = false, readFailed = false;
        SubtitleList cues;
    };

    std::mutex araResultLock;
    std::vector<ARAResult> araResults;          // filled by the engine, consumed on the message thread
    std::set<juce::String> araQueued, araSkipped;
    std::map<juce::String, juce::uint32> araRetryAt;
    juce::String araLayoutSignature;
    int araClipCount = 0, araTranscribedCount = 0;
   #endif

    mutable juce::CriticalSection settingsLock;
    Transcriber::Settings settings;
    juce::File modelFile;

    mutable juce::CriticalSection subtitleLock;
    SubtitleList subtitles;
    std::vector<std::pair<double, double>> transcribedRanges;
    std::atomic<int> subtitleVersion { 0 };

    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<bool> hostPlaying { false };
    double currentSampleRate = 48000.0;
    juce::int64 freeRunningSample = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WhisperSubtitlesProcessor)
};
