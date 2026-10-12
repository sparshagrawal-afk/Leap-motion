#include "Transcriber.h"

#include <whisper.h>

#include <algorithm>
#include <thread>

namespace
{
    bool isNonSpeech (const juce::String& text)
    {
        auto t = text.trim();

        if (t.isEmpty())
            return true;

        // Whisper marks silence / sound effects as "[BLANK_AUDIO]", "(music)", "* laughs *", "♪"...
        const auto noise = juce::String::charToString (0x266a) + juce::String::charToString (0x266b) + "*-.,!? ";
        auto stripped = t.removeCharacters (noise);
        if (stripped.isEmpty())
            return true;

        auto bracketed = [&] (juce::juce_wchar open, juce::juce_wchar close)
        {
            return t[0] == open && t.getLastCharacter() == close
                && t.indexOfChar (1, open) < 0;
        };

        return bracketed ('[', ']') || bracketed ('(', ')');
    }

    void silentLog (enum ggml_log_level, const char*, void*) {}
}

Transcriber::Transcriber() : juce::Thread ("Whisper transcriber")
{
    whisper_log_set (silentLog, nullptr);
    startThread (juce::Thread::Priority::normal);
}

Transcriber::~Transcriber()
{
    shutdown();

    if (ctx != nullptr)
        whisper_free (ctx);
}

void Transcriber::shutdown()
{
    cancelRequested = true;
    signalThreadShouldExit();
    jobAvailable.signal();
    stopThread (10000);
}

//==============================================================================
void Transcriber::post (std::function<void()> job)
{
    {
        const juce::ScopedLock sl (queueLock);
        queue.push_back (std::move (job));
    }

    jobAvailable.signal();
}

void Transcriber::run()
{
    while (! threadShouldExit())
    {
        std::function<void()> job;

        {
            const juce::ScopedLock sl (queueLock);

            if (! queue.empty())
            {
                job = std::move (queue.front());
                queue.pop_front();
            }
        }

        if (job)
            job();
        else
            jobAvailable.wait (200);
    }
}

void Transcriber::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock);
    status = s;
}

juce::String Transcriber::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

//==============================================================================
void Transcriber::loadModel (const juce::File& modelFile)
{
    post ([this, modelFile]
    {
        setStatus ("Loading " + modelFile.getFileName() + "...");
        busy = true;

        if (ctx != nullptr)
        {
            whisper_free (ctx);
            ctx = nullptr;
            modelLoaded = false;
        }

        if (! modelFile.existsAsFile())
        {
            setStatus ("Model not found: " + modelFile.getFullPathName());
            busy = false;
            return;
        }

        auto cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        cparams.flash_attn = true;

        ctx = whisper_init_from_file_with_params (modelFile.getFullPathName().toRawUTF8(), cparams);

        if (ctx == nullptr)
        {
            // Some backends reject flash attention; retry on the plain path before giving up.
            cparams.flash_attn = false;
            ctx = whisper_init_from_file_with_params (modelFile.getFullPathName().toRawUTF8(), cparams);
        }

        modelLoaded = ctx != nullptr;
        setStatus (modelLoaded ? "Model ready: " + modelFile.getFileName()
                               : "Could not load " + modelFile.getFileName() + " (is it a ggml Whisper model?)");
        busy = false;
    });
}

void Transcriber::transcribe (double startSeconds, std::vector<float> audio16k, const Settings& settings)
{
    ++pendingJobs;

    post ([this, startSeconds, audio = std::move (audio16k), settings]
    {
        if (! cancelRequested)
            runTranscription (startSeconds, audio, settings);

        --pendingJobs;

        if (pendingJobs.load() == 0)
            cancelRequested = false;
    });
}

void Transcriber::cancelAll()
{
    if (pendingJobs.load() > 0)
        cancelRequested = true;
}

//==============================================================================
void Transcriber::runTranscription (double startSeconds, const std::vector<float>& audio, const Settings& settings)
{
    if (ctx == nullptr)
    {
        setStatus ("Load a Whisper model first");
        return;
    }

    if (audio.empty())
        return;

    const double lengthSeconds = (double) audio.size() / 16000.0;

    busy = true;
    progress = 0.0f;
    setStatus ("Transcribing " + juce::String (lengthSeconds, 1) + " s starting at "
               + Subtitles::formatTimestamp (startSeconds, '.') + "...");

    auto params = whisper_full_default_params (WHISPER_SAMPLING_BEAM_SEARCH);
    params.beam_search.beam_size = 5;
    params.n_threads = (int) juce::jlimit (1u, 8u, std::max (1u, std::thread::hardware_concurrency()) - 1u);

    const auto language = settings.language.isEmpty() ? juce::String ("auto") : settings.language;
    params.language = language.toRawUTF8();
    params.detect_language = false;
    params.translate = settings.translateToEnglish;

    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;

    params.no_context = true;
    params.suppress_blank = true;
    params.suppress_nst = settings.dropNonSpeech;

    if (settings.maxCharsPerCue > 0)
    {
        params.token_timestamps = true;
        params.max_len = settings.maxCharsPerCue;
        params.split_on_word = true;
    }

    const auto prompt = settings.prompt.trim();
    if (prompt.isNotEmpty())
        params.initial_prompt = prompt.toRawUTF8();

    params.progress_callback = [] (whisper_context*, whisper_state*, int p, void* user)
    {
        static_cast<Transcriber*> (user)->progress = (float) p / 100.0f;
    };
    params.progress_callback_user_data = this;

    params.abort_callback = [] (void* user)
    {
        auto* self = static_cast<Transcriber*> (user);
        return self->cancelRequested.load() || self->threadShouldExit();
    };
    params.abort_callback_user_data = this;

    const auto rc = whisper_full (ctx, params, audio.data(), (int) audio.size());

    if (rc != 0 || cancelRequested || threadShouldExit())
    {
        setStatus (cancelRequested ? "Cancelled" : "Transcription failed (code " + juce::String (rc) + ")");
        busy = false;
        return;
    }

    SubtitleList cues;
    const int n = whisper_full_n_segments (ctx);

    for (int i = 0; i < n; ++i)
    {
        auto text = juce::String::fromUTF8 (whisper_full_get_segment_text (ctx, i)).trim();

        if (settings.dropNonSpeech && isNonSpeech (text))
            continue;

        // whisper times are in 10 ms units, relative to the start of this audio
        const double t0 = (double) whisper_full_get_segment_t0 (ctx, i) / 100.0;
        const double t1 = (double) whisper_full_get_segment_t1 (ctx, i) / 100.0;

        cues.push_back ({ startSeconds + juce::jlimit (0.0, lengthSeconds, t0),
                          startSeconds + juce::jlimit (0.0, lengthSeconds, juce::jmax (t0, t1)),
                          text });
    }

    progress = 1.0f;
    setStatus ("Transcribed " + juce::String ((int) cues.size()) + " cues from "
               + Subtitles::formatTimestamp (startSeconds, '.'));

    if (onResult)
        onResult (startSeconds, startSeconds + lengthSeconds, std::move (cues));

    busy = false;
}
