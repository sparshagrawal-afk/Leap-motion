#pragma once

// ARA lets the DAW hand the plugin whole audio clips (with their position on the
// timeline) instead of streaming them during playback. The ARA build therefore
// transcribes clips without anyone pressing play, and the subtitles follow clips
// that are moved, trimmed or copied.

#include "Subtitles.h"

#include <juce_audio_processors/juce_audio_processors.h>

#if JucePlugin_Enable_ARA

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <map>
#include <memory>

/** Owns the transcription of each audio file (ARA "audio source") in the DAW document,
    timed against the file itself, and saves it with the DAW project. */
class SubtitleDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    struct SourceCues
    {
        SubtitleList cues;          // seconds from the start of the audio file
        juce::String settingsKey;   // which settings produced them (see settingsKeyFor)
    };

    bool getSourceCues (const juce::String& sourceId, SourceCues& result) const;
    void setSourceCues (const juce::String& sourceId, SourceCues cues);
    void clearAllSourceCues();

    /** Changes whenever cues change or the host edits the document (clips moved etc.). */
    int getVersion() const                          { return version.load(); }

    juce::ScopedTryReadLock getProcessingLock()     { return juce::ScopedTryReadLock { processBlockLock }; }

protected:
    void willBeginEditing (juce::ARADocument*) override;
    void didEndEditing (juce::ARADocument*) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    bool doRestoreObjectsFromStream (juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override;
    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override;

private:
    mutable juce::CriticalSection cueLock;
    std::map<juce::String, SourceCues> cuesBySource;
    std::atomic<int> version { 0 };
    juce::ReadWriteLock processBlockLock;
};

#endif

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace ARASubtitles
{
    /** Reads a whole clip's audio file as 16 kHz mono for Whisper. Call on a background thread.
        Returns an empty vector if the host doesn't currently allow reading it. */
    std::vector<float> readForWhisper (juce::AudioFormatReader& reader);
}
