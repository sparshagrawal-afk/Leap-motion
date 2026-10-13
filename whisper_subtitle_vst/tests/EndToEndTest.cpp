// Headless test: drives the plugin like a DAW would (fake transport + playhead),
// checks timeline-aligned capture, and - if a model path is given - runs Whisper.
//
//   WhisperSubtitlesTest [path/to/ggml-model.bin] [path/to/speech.wav]

#include "PluginProcessor.h"
#include "ARASupport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    struct FakePlayHead : juce::AudioPlayHead
    {
        juce::int64 sample = 0;
        bool playing = false;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSamples (sample);
            info.setIsPlaying (playing);
            return info;
        }
    };

    bool waitFor (std::function<bool()> condition, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < end)
        {
            if (condition())
                return true;

            juce::Thread::sleep (20);
        }

        return condition();
    }

    /** Plays 'mono' (at hostRate) through the processor starting at timeline second 'startSeconds'. */
    void play (WhisperSubtitlesProcessor& proc, FakePlayHead& head, const std::vector<float>& mono,
               double hostRate, double startSeconds, bool stopAfter = true)
    {
        constexpr int blockSize = 512;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        head.playing = true;
        head.sample = (juce::int64) std::llround (startSeconds * hostRate);

        for (size_t pos = 0; pos < mono.size(); pos += blockSize)
        {
            const int n = (int) std::min<size_t> (blockSize, mono.size() - pos);
            buffer.setSize (2, n, false, false, true);
            buffer.copyFrom (0, 0, mono.data() + pos, n);
            buffer.copyFrom (1, 0, mono.data() + pos, n);

            proc.processBlock (buffer, midi);

            // pass-through check on the fly
            if (std::abs (buffer.getSample (0, n - 1) - mono[pos + (size_t) n - 1]) > 1.0e-6f)
                check (false, "audio passes through unchanged");

            head.sample += n;

            if (pos % (blockSize * 64) == 0)
                juce::Thread::sleep (1); // roughly real-time-ish pacing so the FIFO is exercised
        }

        if (stopAfter)
        {
            head.playing = false;
            buffer.setSize (2, blockSize);
            buffer.clear();
            proc.processBlock (buffer, midi);
        }
    }

    double correlation (const float* a, const float* b, size_t n)
    {
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < n; ++i) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return ab / std::sqrt (aa * bb + 1e-12);
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File modelFile = argc > 1 ? juce::File (argv[1]) : juce::File();
    const juce::File wavFile = argc > 2 ? juce::File (argv[2])
                                        : juce::File (WHISPER_SAMPLES_DIR).getChildFile ("jfk.wav");

    //==============================================================================
    std::cout << "Subtitle formatting" << std::endl;
    {
        SubtitleList list { { 3.5, 6.0, "Second line" }, { 0.25, 4.0, "First line" }, { 7.0, 7.0, "  " } };
        auto srt = Subtitles::toSrt (list);
        check (srt.startsWith ("1\n00:00:00,250 --> 00:00:03,500\nFirst line\n\n2\n00:00:03,500 --> 00:00:06,000\nSecond line"),
               "SRT sorted, overlaps trimmed, empty cues dropped");

        auto parsed = Subtitles::fromSrt (srt);
        check (parsed.size() == 2 && parsed[1].text == "Second line" && std::abs (parsed[0].start - 0.25) < 1e-9,
               "SRT round-trips");

        check (Subtitles::toVtt (list, 0.25).startsWith ("WEBVTT\n\n00:00:00.000 --> 00:00:03.250"),
               "VTT with export offset");

        check (Subtitles::formatTimestamp (3723.4567, ',') == "01:02:03,457", "timestamp HH:MM:SS,mmm");

        // ARA: a clip that plays 10..20 s of its file at timeline 100..110 s
        SubtitleList fileCues { { 1, 3, "before clip" }, { 9, 12, "straddles start" }, { 15, 16, "inside" }, { 19.5, 25, "straddles end" } };
        auto placed = Subtitles::mapSourceToTimeline (fileCues, 10.0, 20.0, 100.0, 110.0);
        check (placed.size() == 3
                 && std::abs (placed[0].start - 100.0) < 1e-9 && std::abs (placed[0].end - 102.0) < 1e-9
                 && std::abs (placed[1].start - 105.0) < 1e-9 && placed[1].text == "inside"
                 && std::abs (placed[2].end - 110.0) < 1e-9,
               "ARA clip cues are trimmed to the clip and moved to its timeline position");

        auto stretched = Subtitles::mapSourceToTimeline ({ { 15, 16, "x" } }, 10.0, 20.0, 100.0, 120.0);
        check (stretched.size() == 1 && std::abs (stretched[0].start - 110.0) < 1e-9 && std::abs (stretched[0].end - 112.0) < 1e-9,
               "a time-stretched clip scales cue times");

        SubtitleList target { { 0, 2, "keep" }, { 10, 12, "old take" }, { 20, 22, "keep too" } };
        Subtitles::replaceRange (target, 9, 15, { { 9.5, 11, "new take" } });
        check (target.size() == 3 && target[1].text == "new take", "re-transcribing a range replaces old cues");
    }

    //==============================================================================
    std::cout << "Capture against the host timeline" << std::endl;

    std::vector<float> speech16k;
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));

        if (reader == nullptr)
        {
            std::cout << "Could not read " << wavFile.getFullPathName() << std::endl;
            return 1;
        }

        juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
        check (std::abs (reader->sampleRate - 16000.0) < 1, "test speech is 16 kHz (" + wavFile.getFileName() + ")");
        speech16k.assign (buf.getReadPointer (0), buf.getReadPointer (0) + buf.getNumSamples());
    }

    // Simulate a 48 kHz session: upsample x3
    const double hostRate = 48000.0;
    std::vector<float> speech48k (speech16k.size() * 3);
    for (size_t i = 0; i < speech48k.size(); ++i)
    {
        const double p = (double) i / 3.0;
        const auto j = (size_t) p;
        const float a = speech16k[j], b = speech16k[std::min (j + 1, speech16k.size() - 1)];
        speech48k[i] = a + (b - a) * (float) (p - (double) j);
    }

    // ARA reads whole files: check a 48 kHz stereo file comes out as the same 16 kHz mono speech
    {
        juce::MemoryBlock wavData;
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (wavData, false);
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (hostRate)
                                                                                         .withNumChannels (2)
                                                                                         .withBitsPerSample (24));
            juce::AudioBuffer<float> stereo (2, (int) speech48k.size());
            stereo.copyFrom (0, 0, speech48k.data(), (int) speech48k.size());
            stereo.copyFrom (1, 0, speech48k.data(), (int) speech48k.size());
            writer->writeFromAudioSampleBuffer (stereo, 0, stereo.getNumSamples());
        } // writer finalises the WAV header when destroyed

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::MemoryInputStream (wavData, false), true));
        const auto audio = ARASubtitles::readForWhisper (*reader);

        check (std::abs ((double) audio.size() - (double) speech16k.size()) < 4,
               "ARA file reader: 48 kHz stereo -> 16 kHz mono length (" + juce::String ((int) audio.size()) + " samples)");

        double best = 0;
        const auto n = std::min (audio.size(), speech16k.size()) - 32;
        for (int lag = 0; lag < 6; ++lag)
            best = std::max (best, correlation (audio.data() + lag, speech16k.data(), n - (size_t) lag));
        check (best > 0.95, "ARA file reader: audio matches the source (corr " + juce::String (best, 4) + ")");
    }

    WhisperSubtitlesProcessor proc;
    FakePlayHead head;
    proc.setPlayHead (&head);
    proc.setPlayConfigDetails (2, 2, hostRate, 512);
    proc.prepareToPlay (hostRate, 512);
    proc.autoTranscribe = modelFile.existsAsFile();

    const double dialogueStart = 3600.0 + 12.5; // e.g. a session starting at 01:00:00:00
    play (proc, head, speech48k, hostRate, dialogueStart);

    check (waitFor ([&] { return proc.capture.getRegionsSnapshot().size() == 1 && ! proc.capture.isRecording(); }, 5000),
           "one region captured and closed when transport stops");

    auto regions = proc.capture.getRegionsSnapshot();
    if (regions.size() == 1)
    {
        auto& r = regions[0];
        check (std::abs (r.startSeconds - dialogueStart) < 1.0e-6, "region starts at the playhead position ("
                                                                    + juce::String (r.startSeconds, 3) + " s)");
        check (std::abs (r.lengthSeconds() - (double) speech16k.size() / 16000.0) < 0.01,
               "region length matches the audio (" + juce::String (r.lengthSeconds(), 3) + " s)");

        const auto n = std::min (r.samples.size(), speech16k.size()) - 32;
        // linear upsample + anti-alias filter delay the signal by a sample or two; take the best lag
        double best = 0;
        for (int lag = 0; lag < 6; ++lag)
            best = std::max (best, correlation (r.samples.data() + lag, speech16k.data(), n - (size_t) lag));
        check (best > 0.95, "resampled audio matches the source (corr " + juce::String (best, 4) + ")");
    }

    // Playhead jump: play 2 s, jump, play 2 s -> two regions
    std::vector<float> tone ((size_t) (hostRate * 2.0));
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) i / hostRate);

    play (proc, head, tone, hostRate, 200.0, false);
    play (proc, head, tone, hostRate, 300.0, true);
    check (waitFor ([&] { return proc.capture.getRegionsSnapshot().size() == 3; }, 5000),
           "a playhead jump starts a new region");

    proc.capture.armed = false;
    play (proc, head, tone, hostRate, 400.0, true);
    juce::Thread::sleep (200);
    check (proc.capture.getRegionsSnapshot().size() == 3, "nothing is captured while disarmed");
    proc.capture.armed = true;

    // Replaying the same passage replaces the old take
    play (proc, head, tone, hostRate, 200.0, true);
    check (waitFor ([&] { return proc.capture.getRegionsSnapshot().size() == 3; }, 5000),
           "a second pass over the same range supersedes the first");

    // State save/restore keeps settings + cues
    {
        auto s = proc.getSettings();
        s.language = "ml";              // Malayalam dialogue -> English subtitles
        s.translateToEnglish = true;
        s.prompt = "Ask not";
        proc.setSettings (s);
        proc.setSubtitles ({ { 1, 2, "hello" } });

        juce::MemoryBlock state;
        proc.getStateInformation (state);

        WhisperSubtitlesProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        auto subs = restored.getSubtitles();
        check (restored.getSettings().language == "ml" && restored.getSettings().translateToEnglish
                   && restored.getSettings().prompt == "Ask not"
                   && subs.size() == 1 && subs[0].text == "hello",
               "DAW session state restores settings and subtitles");
        proc.clearSubtitles();
    }

    //==============================================================================
    if (! modelFile.existsAsFile())
    {
        std::cout << "\nNo model given - skipping Whisper transcription.\n"
                     "Run: WhisperSubtitlesTest path/to/ggml-base.en.bin" << std::endl;
        return failures == 0 ? 0 : 1;
    }

    std::cout << "Whisper transcription (" << modelFile.getFileName() << ")" << std::endl;
    proc.setModelFile (modelFile);
    check (waitFor ([&] { return proc.transcriber->isModelLoaded(); }, 60000), "model loads");

    proc.transcribeCaptured (false);
    check (waitFor ([&] { return ! proc.transcriber->isBusy() && ! proc.getSubtitles().empty(); }, 300000),
           "transcription finishes");

    const auto subs = proc.getSubtitles();
    std::cout << "\n" << Subtitles::toSrt (subs, 3600.0) << std::endl;

    juce::String all;
    for (auto& s : subs)
        all << s.text << " ";

    if (wavFile.getFileName() == "jfk.wav")
        check (all.containsIgnoreCase ("country"), "recognised the JFK quote");

    check (! subs.empty() && subs.front().start >= dialogueStart - 0.01 && subs.front().start < dialogueStart + 2.0,
           "first cue sits at the dialogue's timeline position");

    std::cout << (failures == 0 ? "\nALL PASSED" : "\nFAILURES: " + std::to_string (failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
