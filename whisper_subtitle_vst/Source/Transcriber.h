#pragma once

#include "Subtitles.h"

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <vector>

struct whisper_context;

/** Runs whisper.cpp on a background thread. All callbacks fire on that thread. */
class Transcriber : private juce::Thread
{
public:
    struct Settings
    {
        juce::String language { "auto" };   // ISO code ("en", "es", ...) or "auto"
        bool translateToEnglish = false;
        int maxCharsPerCue = 42;            // 0 = let Whisper choose segment length
        juce::String prompt;                // names / jargon that help recognition
        bool dropNonSpeech = true;          // remove cues like "[Music]" or "(applause)"
    };

    Transcriber();
    ~Transcriber() override;

    void loadModel (const juce::File& modelFile);
    void transcribe (double startSeconds, std::vector<float> audio16k, const Settings&);
    void cancelAll();

    /** Stops the worker thread; no callbacks fire afterwards. */
    void shutdown();

    bool isModelLoaded() const      { return modelLoaded.load(); }
    bool isBusy() const             { return busy.load() || pendingJobs.load() > 0; }
    int getPendingJobs() const      { return pendingJobs.load(); }
    float getProgress() const       { return progress.load(); }
    juce::String getStatus() const;

    /** (regionStart, regionEnd, cues on the host timeline) */
    std::function<void (double, double, SubtitleList)> onResult;

private:
    void run() override;
    void post (std::function<void()> job);
    void setStatus (const juce::String&);
    void runTranscription (double startSeconds, const std::vector<float>& audio, const Settings&);

    whisper_context* ctx = nullptr;

    juce::CriticalSection queueLock;
    std::deque<std::function<void()>> queue;
    juce::WaitableEvent jobAvailable;

    std::atomic<bool> modelLoaded { false }, busy { false }, cancelRequested { false };
    std::atomic<int> pendingJobs { 0 };
    std::atomic<float> progress { 0.0f };

    mutable juce::CriticalSection statusLock;
    juce::String status { "No model loaded" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transcriber)
};
