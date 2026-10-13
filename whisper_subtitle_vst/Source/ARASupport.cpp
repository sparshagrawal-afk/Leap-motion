#include "ARASupport.h"
#include "Resampler.h"

//==============================================================================
std::vector<float> ARASubtitles::readForWhisper (juce::AudioFormatReader& reader)
{
    std::vector<float> out;

    const auto total = reader.lengthInSamples;
    const int channels = juce::jlimit (1, 64, (int) reader.numChannels);

    if (total <= 0 || reader.sampleRate <= 0.0)
        return out;

    out.reserve ((size_t) ((double) total * WhisperResampler::outputRate / reader.sampleRate) + 16);

    WhisperResampler resampler;
    resampler.prepare (reader.sampleRate);

    constexpr int chunk = 1 << 16;
    juce::AudioBuffer<float> buffer (channels, chunk);
    std::vector<float> mono ((size_t) chunk);

    for (juce::int64 pos = 0; pos < total; pos += chunk)
    {
        const int n = (int) juce::jmin ((juce::int64) chunk, total - pos);

        if (! reader.read (&buffer, 0, n, pos, true, true))
            return {}; // host withdrew sample access; try again later

        juce::FloatVectorOperations::copyWithMultiply (mono.data(), buffer.getReadPointer (0), 1.0f / (float) channels, n);

        for (int ch = 1; ch < channels; ++ch)
            juce::FloatVectorOperations::addWithMultiply (mono.data(), buffer.getReadPointer (ch), 1.0f / (float) channels, n);

        resampler.process (mono.data(), n, out);
    }

    return out;
}

#if JucePlugin_Enable_ARA

namespace
{
    constexpr int archiveVersion = 1;

    /** Shared thread that reads ahead from clips so playback never waits on the disk. */
    class SharedReadAheadThread final : public juce::TimeSliceThread
    {
    public:
        SharedReadAheadThread() : juce::TimeSliceThread ("Whisper Subtitles ARA read-ahead")
        {
            startThread (juce::Thread::Priority::high);
        }
    };

    //==============================================================================
    /** Plays the clips back. In ARA mode the host doesn't feed the clip audio through the
        plugin's input: the plugin renders it itself, so this must reproduce it exactly. */
    class SubtitlePlaybackRenderer final : public juce::ARAPlaybackRenderer
    {
    public:
        SubtitlePlaybackRenderer (ARA::PlugIn::DocumentController* dc, SubtitleDocumentController& ownerIn)
            : juce::ARAPlaybackRenderer (dc), owner (ownerIn) {}

        void prepareToPlay (double sampleRateIn, int maximumSamplesPerBlockIn, int numChannelsIn,
                            juce::AudioProcessor::ProcessingPrecision, AlwaysNonRealtime alwaysNonRealtime) override
        {
            sampleRate = sampleRateIn;
            maximumSamplesPerBlock = maximumSamplesPerBlockIn;
            numChannels = numChannelsIn;
            mixBuffer.setSize (numChannels, maximumSamplesPerBlock);

            const bool buffered = alwaysNonRealtime == AlwaysNonRealtime::no;
            readers.clear();

            for (auto* region : getPlaybackRegions())
            {
                auto* source = region->getAudioModification()->getAudioSource();

                if (readers.count (source) != 0)
                    continue;

                auto reader = std::make_unique<juce::ARAAudioSourceReader> (source);
                Reader r;

                if (buffered)
                {
                    const auto readAhead = juce::jmax (4 * maximumSamplesPerBlock, juce::roundToInt (2.0 * sampleRate));
                    auto bufferingReader = std::make_unique<juce::BufferingAudioReader> (reader.release(), *readAheadThread, readAhead);
                    r.buffering = bufferingReader.get();
                    r.reader = std::move (bufferingReader);
                }
                else
                {
                    r.reader = std::move (reader);
                }

                readers.emplace (source, std::move (r));
            }
        }

        void releaseResources() override
        {
            readers.clear();
        }

        bool processBlock (juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime realtime,
                           const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override
        {
            const auto lock = owner.getProcessingLock();

            if (! lock.isLocked()) // host is editing the document right now
            {
                buffer.clear();
                return true;
            }

            const int numSamples = buffer.getNumSamples();
            bool success = true, renderedAny = false;

            if (positionInfo.getIsPlaying())
            {
                const auto blockRange = juce::Range<juce::int64>::withStartAndLength (positionInfo.getTimeInSamples().orFallback (0),
                                                                                    numSamples);

                for (auto* region : getPlaybackRegions())
                {
                    const auto playbackRange = region->getSampleRange (sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
                    auto renderRange = blockRange.getIntersectionWith (playbackRange);

                    if (renderRange.isEmpty())
                        continue;

                    const juce::Range<juce::int64> modificationRange { region->getStartInAudioModificationSamples(),
                                                                       region->getEndInAudioModificationSamples() };
                    const auto offsetToSource = modificationRange.getStart() - playbackRange.getStart();
                    renderRange = renderRange.getIntersectionWith (modificationRange.movedToStartAt (playbackRange.getStart()));

                    if (renderRange.isEmpty())
                        continue;

                    auto* source = region->getAudioModification()->getAudioSource();
                    const auto it = readers.find (source);

                    // Clips at a different sample rate would need resampling; the host normally converts.
                    if (it == readers.end() || ! juce::approximatelyEqual (source->getSampleRate(), sampleRate))
                    {
                        success = false;
                        continue;
                    }

                    if (it->second.buffering != nullptr)
                        it->second.buffering->setReadTimeout (realtime == juce::AudioProcessor::Realtime::no ? 100 : 0);

                    const int count = (int) renderRange.getLength();
                    const int startInBuffer = (int) (renderRange.getStart() - blockRange.getStart());
                    auto& target = renderedAny ? mixBuffer : buffer;

                    // AudioFormatReader::read duplicates a mono file onto both channels of a stereo track.
                    if (! it->second.reader->read (&target, startInBuffer, count,
                                                   renderRange.getStart() + offsetToSource, true, true))
                    {
                        success = false;
                        continue;
                    }

                    if (renderedAny)
                    {
                        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                            buffer.addFrom (ch, startInBuffer, mixBuffer, ch, startInBuffer, count);
                    }
                    else
                    {
                        if (startInBuffer > 0)
                            buffer.clear (0, startInBuffer);

                        const int end = startInBuffer + count;
                        if (end < numSamples)
                            buffer.clear (end, numSamples - end);

                        renderedAny = true;
                    }
                }
            }

            if (! renderedAny)
                buffer.clear();

            return success;
        }

        using juce::ARAPlaybackRenderer::processBlock;

    private:
        struct Reader
        {
            std::unique_ptr<juce::AudioFormatReader> reader;
            juce::BufferingAudioReader* buffering = nullptr;
        };

        SubtitleDocumentController& owner;
        juce::SharedResourcePointer<SharedReadAheadThread> readAheadThread;
        std::map<juce::ARAAudioSource*, Reader> readers;
        juce::AudioBuffer<float> mixBuffer;
        double sampleRate = 44100.0;
        int maximumSamplesPerBlock = 512, numChannels = 2;
    };
}

//==============================================================================
bool SubtitleDocumentController::getSourceCues (const juce::String& sourceId, SourceCues& result) const
{
    const juce::ScopedLock sl (cueLock);
    const auto it = cuesBySource.find (sourceId);

    if (it == cuesBySource.end())
        return false;

    result = it->second;
    return true;
}

void SubtitleDocumentController::setSourceCues (const juce::String& sourceId, SourceCues cues)
{
    {
        const juce::ScopedLock sl (cueLock);
        cuesBySource[sourceId] = std::move (cues);
    }

    ++version;
}

void SubtitleDocumentController::clearAllSourceCues()
{
    {
        const juce::ScopedLock sl (cueLock);
        cuesBySource.clear();
    }

    ++version;
}

void SubtitleDocumentController::willBeginEditing (juce::ARADocument*)
{
    processBlockLock.enterWrite();
}

void SubtitleDocumentController::didEndEditing (juce::ARADocument*)
{
    processBlockLock.exitWrite();
    ++version;
}

juce::ARAPlaybackRenderer* SubtitleDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new SubtitlePlaybackRenderer (getDocumentController(), *this);
}

//==============================================================================
// Archive: version, number of files, then per file: id, settings key, cue count, cues.
bool SubtitleDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                         const juce::ARAStoreObjectsFilter* filter) noexcept
{
    std::vector<std::pair<juce::String, SourceCues>> toStore;

    {
        const juce::ScopedLock sl (cueLock);

        for (auto* source : filter->getAudioSourcesToStore<juce::ARAAudioSource>())
        {
            const juce::String id (source->getPersistentID());
            const auto it = cuesBySource.find (id);

            if (it != cuesBySource.end())
                toStore.emplace_back (id, it->second);
        }
    }

    bool ok = output.writeInt (archiveVersion)
           && output.writeInt64 ((juce::int64) toStore.size());

    for (auto& [id, entry] : toStore)
    {
        ok = ok && output.writeString (id)
                && output.writeString (entry.settingsKey)
                && output.writeInt64 ((juce::int64) entry.cues.size());

        for (auto& cue : entry.cues)
            ok = ok && output.writeDouble (cue.start)
                    && output.writeDouble (cue.end)
                    && output.writeString (cue.text);
    }

    return ok;
}

bool SubtitleDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                                             const juce::ARARestoreObjectsFilter* filter) noexcept
{
    if (input.readInt() != archiveVersion)
        return false;

    const auto numSources = input.readInt64();

    for (juce::int64 i = 0; i < numSources && ! input.failed(); ++i)
    {
        const auto id = input.readString();
        SourceCues entry;
        entry.settingsKey = input.readString();

        const auto numCues = input.readInt64();

        for (juce::int64 c = 0; c < numCues && ! input.failed(); ++c)
        {
            SubtitleSegment cue;
            cue.start = input.readDouble();
            cue.end = input.readDouble();
            cue.text = input.readString();
            entry.cues.push_back (std::move (cue));
        }

        // The filter maps archived ids to the sources being restored (ids can change on import).
        if (auto* source = filter->getAudioSourceToRestoreStateWithID<juce::ARAAudioSource> (id.toRawUTF8()))
            setSourceCues (juce::String (source->getPersistentID()), std::move (entry));
    }

    return ! input.failed();
}

//==============================================================================
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<SubtitleDocumentController>();
}

#endif
