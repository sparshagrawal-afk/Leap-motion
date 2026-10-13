#pragma once

#include "Subtitles.h"

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <vector>

struct whisper_context;

/** Runs whisper.cpp on a background thread. All callbacks fire on that thread.

    Plugin instances share one engine (juce::SharedResourcePointer<Transcriber>),
    so a large model is only loaded into memory once. */
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

    /** Identifies the settings that produced a transcription (to know when to redo it). */
    static juce::String settingsKey (const Settings&);

    Transcriber();
    ~Transcriber() override;

    /** (ok, start, end, cues on the host timeline). ok is false if cancelled, failed or no model. */
    using ResultCallback = std::function<void (bool, double, double, SubtitleList)>;

    /** Produces 16 kHz mono audio; called on the worker thread right before transcribing. */
    using AudioProvider = std::function<std::vector<float>()>;

    /** Loads a ggml model, unless that file is already loaded or loading. */
    void loadModel (const juce::File& modelFile);
    juce::File getModelFile() const;

    void transcribe (double startSeconds, std::vector<float> audio16k, const Settings&, ResultCallback);
    void transcribe (double startSeconds, AudioProvider, const Settings&, ResultCallback);
    void cancelAll();

    /** Stops the worker thread; no callbacks fire afterwards. */
    void shutdown();

    bool isModelLoaded() const      { return modelLoaded.load(); }
    bool isBusy() const             { return busy.load() || pendingJobs.load() > 0; }
    int getPendingJobs() const      { return pendingJobs.load(); }
    float getProgress() const       { return progress.load(); }
    juce::String getStatus() const;

private:
    void run() override;
    void post (std::function<void()> job);
    void setStatus (const juce::String&);
    struct Result
    {
        bool ok = false;
        double endSeconds = 0.0;
        SubtitleList cues;
    };

    Result runTranscription (double startSeconds, const std::vector<float>& audio, const Settings&);

    whisper_context* ctx = nullptr;

    juce::CriticalSection queueLock;
    std::deque<std::function<void()>> queue;
    juce::WaitableEvent jobAvailable;

    std::atomic<bool> modelLoaded { false }, busy { false }, cancelRequested { false };
    std::atomic<int> pendingJobs { 0 };
    std::atomic<float> progress { 0.0f };

    mutable juce::CriticalSection statusLock;
    juce::String status { "No model loaded" };
    juce::File requestedModel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transcriber)
};
