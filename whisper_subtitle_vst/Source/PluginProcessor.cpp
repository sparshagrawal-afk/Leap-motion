#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier stateId ("WhisperSubtitlesState"),
                           modelId ("modelPath"), languageId ("language"), translateId ("translate"),
                           maxCharsId ("maxChars"), promptId ("prompt"), dropNonSpeechId ("dropNonSpeech"),
                           autoId ("autoTranscribe"), armedId ("armed"), offsetId ("exportOffset"),
                           compactId ("compactView"), compactWId ("compactWidth"), compactHId ("compactHeight");
}

WhisperSubtitlesProcessor::WhisperSubtitlesProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    capture.onRegionFinished = [this] (const AudioCapture::Region& r) { handleRegionFinished (r); };

   #if JucePlugin_Enable_ARA
    startTimerHz (2); // picks up new clips, finished transcriptions and moved clips
   #endif

    // After the host has restored any saved state: if no model was chosen, use the one the
    // engine already has (another instance loaded it) or the one the installer downloaded.
    juce::MessageManager::callAsync ([this, weak = std::weak_ptr<Lifetime> (lifetime)]
    {
        auto l = weak.lock();

        if (l == nullptr || ! l->alive || getModelFile() != juce::File())
            return;

        if (auto loaded = transcriber->getModelFile(); loaded != juce::File())
        {
            const juce::ScopedLock sl (settingsLock);
            modelFile = loaded;
        }
        else if (auto installed = Transcriber::findInstalledModel(); installed != juce::File())
        {
            setModelFile (installed);
        }
    });
}

WhisperSubtitlesProcessor::~WhisperSubtitlesProcessor()
{
    stopTimer();

    // Stop the capture thread, then make sure no transcription result reaches us
    // from the shared engine once our members are gone.
    capture.shutdown();

    const std::lock_guard<std::mutex> lock (lifetime->lock);
    lifetime->alive = false;
}

Transcriber::ResultCallback WhisperSubtitlesProcessor::guarded (Transcriber::ResultCallback fn)
{
    return [weak = std::weak_ptr<Lifetime> (lifetime), fn = std::move (fn)] (bool ok, double s, double e, SubtitleList cues)
    {
        if (auto l = weak.lock())
        {
            const std::lock_guard<std::mutex> lock (l->lock);

            if (l->alive)
                fn (ok, s, e, std::move (cues));
        }
    };
}

//==============================================================================
bool WhisperSubtitlesProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void WhisperSubtitlesProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    capture.prepare (sampleRate, samplesPerBlock);

   #if JucePlugin_Enable_ARA
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
   #endif
}

void WhisperSubtitlesProcessor::releaseResources()
{
   #if JucePlugin_Enable_ARA
    releaseResourcesForARA();
   #endif
}

double WhisperSubtitlesProcessor::getTailLengthSeconds() const
{
   #if JucePlugin_Enable_ARA
    double tail = 0.0;
    if (getTailLengthSecondsForARA (tail))
        return tail;
   #endif

    return 0.0;
}

bool WhisperSubtitlesProcessor::isUsingARA() const
{
   #if JucePlugin_Enable_ARA
    return isBoundToARA();
   #else
    return false;
   #endif
}

void WhisperSubtitlesProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    bool playing = false;
    juce::int64 timelineSample = freeRunningSample;

    if (auto* head = getPlayHead())
    {
        if (auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying() || pos->getIsRecording();

            if (auto samples = pos->getTimeInSamples())
                timelineSample = *samples;
            else if (auto seconds = pos->getTimeInSeconds())
                timelineSample = (juce::int64) std::llround (*seconds * currentSampleRate);
        }
    }
    else
    {
        playing = true; // no host transport (e.g. some standalone setups): treat as always rolling
    }

    freeRunningSample = timelineSample + buffer.getNumSamples();
    playheadSeconds.store ((double) timelineSample / currentSampleRate, std::memory_order_relaxed);
    hostPlaying.store (playing, std::memory_order_relaxed);

   #if JucePlugin_Enable_ARA
    // In ARA mode we render the clips ourselves and read them directly; nothing to capture.
    if (processBlockForARA (buffer, isRealtime(), getPlayHead()))
        return;
   #endif

    // Audio passes through untouched; we only listen.
    capture.push (buffer, timelineSample, playing, isNonRealtime());
}

//==============================================================================
void WhisperSubtitlesProcessor::setModelFile (const juce::File& f)
{
    {
        const juce::ScopedLock sl (settingsLock);
        modelFile = f;
    }

    transcriber->loadModel (f);
}

juce::File WhisperSubtitlesProcessor::getModelFile() const
{
    const juce::ScopedLock sl (settingsLock);
    return modelFile;
}

Transcriber::Settings WhisperSubtitlesProcessor::getSettings() const
{
    const juce::ScopedLock sl (settingsLock);
    return settings;
}

void WhisperSubtitlesProcessor::setSettings (const Transcriber::Settings& s)
{
    const juce::ScopedLock sl (settingsLock);
    settings = s;
}

//==============================================================================
void WhisperSubtitlesProcessor::handleRegionFinished (const AudioCapture::Region& region)
{
    if (autoTranscribe.load())
        transcriber->transcribe (region.startSeconds, region.samples, getSettings(),
                                 guarded ([this] (bool ok, double s, double e, SubtitleList cues)
                                 {
                                     if (ok)
                                         handleTranscription (s, e, std::move (cues));
                                 }));
}

void WhisperSubtitlesProcessor::transcribeCaptured (bool all)
{
   #if JucePlugin_Enable_ARA
    if (isUsingARA())
    {
        transcribeARAClips (all);
        return;
    }
   #endif

    const auto snapshot = capture.getRegionsSnapshot();
    const auto s = getSettings();

    std::vector<std::pair<double, double>> done;
    {
        const juce::ScopedLock sl (subtitleLock);
        done = transcribedRanges;
    }

    for (auto& region : snapshot)
    {
        const bool alreadyDone = std::any_of (done.begin(), done.end(), [&] (const auto& r)
        {
            return std::abs (r.first - region.startSeconds) < 0.05
                && std::abs (r.second - region.endSeconds()) < 0.05;
        });

        if (all || ! alreadyDone)
            transcriber->transcribe (region.startSeconds, region.samples, s,
                                     guarded ([this] (bool ok, double start, double end, SubtitleList cues)
                                     {
                                         if (ok)
                                             handleTranscription (start, end, std::move (cues));
                                     }));
    }
}

void WhisperSubtitlesProcessor::handleTranscription (double regionStart, double regionEnd, SubtitleList cues)
{
    {
        const juce::ScopedLock sl (subtitleLock);
        Subtitles::replaceRange (subtitles, regionStart, regionEnd, cues);
        transcribedRanges.emplace_back (regionStart, regionEnd);
    }

    ++subtitleVersion;
}

SubtitleList WhisperSubtitlesProcessor::getSubtitles() const
{
    const juce::ScopedLock sl (subtitleLock);
    return subtitles;
}

void WhisperSubtitlesProcessor::setSubtitles (SubtitleList list)
{
    {
        const juce::ScopedLock sl (subtitleLock);
        subtitles = Subtitles::normalised (std::move (list));
    }

    ++subtitleVersion;
}

void WhisperSubtitlesProcessor::clearSubtitles()
{
   #if JucePlugin_Enable_ARA
    // Keep an (empty) result per clip so auto-transcribe doesn't immediately redo them.
    if (auto* dc = getARADocumentController())
    {
        const auto key = Transcriber::settingsKey (getSettings());

        for (auto* region : getARARegions())
            dc->setSourceCues (juce::String (region->getAudioModification()->getAudioSource()->getPersistentID()),
                               { {}, key });
    }
   #endif

    {
        const juce::ScopedLock sl (subtitleLock);
        subtitles.clear();
        transcribedRanges.clear();
    }

    ++subtitleVersion;
}

//==============================================================================
void WhisperSubtitlesProcessor::timerCallback()
{
   #if JucePlugin_Enable_ARA
    if (isUsingARA())
        refreshFromARA();
   #endif
}

juce::String WhisperSubtitlesProcessor::getARAStatus() const
{
   #if JucePlugin_Enable_ARA
    if (isUsingARA())
    {
        juce::String s;
        s << "ARA: " << araClipCount << (araClipCount == 1 ? " clip, " : " clips, ")
          << araTranscribedCount << " transcribed";

        if (! araQueued.empty())
            s << ", " << (int) araQueued.size() << " in progress";

        return s;
    }
   #endif

    return {};
}

#if JucePlugin_Enable_ARA
SubtitleDocumentController* WhisperSubtitlesProcessor::getARADocumentController() const
{
    if (auto* dc = getDocumentController())
        return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<SubtitleDocumentController> (dc);

    return nullptr;
}

std::vector<juce::ARAPlaybackRegion*> WhisperSubtitlesProcessor::getARARegions() const
{
    std::vector<juce::ARAPlaybackRegion*> regions;

    auto add = [&regions] (const auto& list)
    {
        for (auto* r : list)
            if (std::find (regions.begin(), regions.end(), r) == regions.end())
                regions.push_back (r);
    };

    // The clips the host assigned to this instance (Logic: the track's regions)...
    if (auto* renderer = getPlaybackRenderer())
        add (renderer->getPlaybackRegions());

    if (auto* renderer = getEditorRenderer())
    {
        add (renderer->getPlaybackRegions());

        for (auto* sequence : renderer->getRegionSequences())
            add (sequence->getPlaybackRegions());
    }

    // ...or, if the host didn't assign any, every clip in the document.
    if (regions.empty())
        if (auto* dc = getARADocumentController())
            if (auto* document = dc->getDocument())
                for (auto* sequence : document->getRegionSequences())
                    add (sequence->getPlaybackRegions());

    return regions;
}

void WhisperSubtitlesProcessor::queueARASource (juce::ARAAudioSource* source, const Transcriber::Settings& s)
{
    const juce::String id (source->getPersistentID());

    if (araQueued.count (id) != 0)
        return;

    araQueued.insert (id);
    araRetryAt.erase (id);

    auto reader = std::make_shared<juce::ARAAudioSourceReader> (source);
    auto readFailed = std::make_shared<std::atomic<bool>> (false);

    auto provider = [reader, readFailed]() mutable
    {
        auto audio = ARASubtitles::readForWhisper (*reader);
        readFailed->store (audio.empty());

        // the reader listens to the host's audio source: let it go on the message thread
        juce::MessageManager::callAsync ([r = std::move (reader)] {});
        return audio;
    };

    transcriber->transcribe (0.0, std::move (provider), s,
                             guarded ([this, id, key = Transcriber::settingsKey (s), readFailed]
                                      (bool ok, double, double, SubtitleList cues)
                                      {
                                          const std::lock_guard<std::mutex> lock (araResultLock);
                                          araResults.push_back ({ id, key, ok, readFailed->load(), std::move (cues) });
                                      }));
}

void WhisperSubtitlesProcessor::transcribeARAClips (bool all)
{
    auto* dc = getARADocumentController();

    if (dc == nullptr)
        return;

    const auto s = getSettings();

    for (auto* region : getARARegions())
    {
        auto* source = region->getAudioModification()->getAudioSource();
        const juce::String id (source->getPersistentID());
        SubtitleDocumentController::SourceCues existing;

        araSkipped.erase (id);

        if (all || ! dc->getSourceCues (id, existing))
            queueARASource (source, s);
    }
}

void WhisperSubtitlesProcessor::refreshFromARA()
{
    auto* dc = getARADocumentController();

    if (dc == nullptr)
        return;

    // 1. Store finished transcriptions with the document.
    std::vector<ARAResult> finished;
    {
        const std::lock_guard<std::mutex> lock (araResultLock);
        finished.swap (araResults);
    }

    for (auto& r : finished)
    {
        araQueued.erase (r.sourceId);

        if (r.ok)
            dc->setSourceCues (r.sourceId, { std::move (r.cues), r.settingsKey });
        else if (r.readFailed)
            araRetryAt[r.sourceId] = juce::Time::getMillisecondCounter() + 5000; // host wasn't ready
        else
            araSkipped.insert (r.sourceId); // cancelled or failed: wait for the Transcribe button
    }

    // 2. Transcribe clips nobody has transcribed yet.
    const auto regions = getARARegions();
    const auto now = juce::Time::getMillisecondCounter();
    std::set<juce::String> sources;
    int transcribed = 0;

    for (auto* region : regions)
    {
        auto* source = region->getAudioModification()->getAudioSource();
        const juce::String id (source->getPersistentID());

        if (! sources.insert (id).second)
            continue;

        SubtitleDocumentController::SourceCues existing;

        if (dc->getSourceCues (id, existing))
        {
            ++transcribed;
            continue;
        }

        const auto retry = araRetryAt.find (id);
        const bool waiting = retry != araRetryAt.end() && now < retry->second;

        if (autoTranscribe.load() && transcriber->isModelLoaded() && ! waiting && araSkipped.count (id) == 0)
            queueARASource (source, getSettings());
    }

    araClipCount = (int) sources.size();
    araTranscribedCount = transcribed;

    // 3. Lay the cues out on the timeline wherever the clips currently sit.
    juce::String signature;
    signature << dc->getVersion();

    for (auto* region : regions)
        signature << "|" << juce::String::toHexString ((juce::pointer_sized_int) region)
                  << ":" << region->getStartInPlaybackTime() << ":" << region->getEndInPlaybackTime();

    if (signature == araLayoutSignature)
        return;

    araLayoutSignature = signature;

    SubtitleList timeline;

    for (auto* region : regions)
    {
        SubtitleDocumentController::SourceCues entry;

        if (! dc->getSourceCues (juce::String (region->getAudioModification()->getAudioSource()->getPersistentID()), entry))
            continue;

        auto placed = Subtitles::mapSourceToTimeline (entry.cues,
                                                      region->getStartInAudioModificationTime(),
                                                      region->getEndInAudioModificationTime(),
                                                      region->getStartInPlaybackTime(),
                                                      region->getEndInPlaybackTime());
        timeline.insert (timeline.end(), placed.begin(), placed.end());
    }

    setSubtitles (std::move (timeline));
}
#endif

//==============================================================================
void WhisperSubtitlesProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state (stateId);

    {
        const juce::ScopedLock sl (settingsLock);
        state.setProperty (modelId, modelFile.getFullPathName(), nullptr);
        state.setProperty (languageId, settings.language, nullptr);
        state.setProperty (translateId, settings.translateToEnglish, nullptr);
        state.setProperty (maxCharsId, settings.maxCharsPerCue, nullptr);
        state.setProperty (promptId, settings.prompt, nullptr);
        state.setProperty (dropNonSpeechId, settings.dropNonSpeech, nullptr);
    }

    state.setProperty (autoId, autoTranscribe.load(), nullptr);
    state.setProperty (armedId, capture.armed.load(), nullptr);
    state.setProperty (offsetId, exportOffsetSeconds.load(), nullptr);
    state.setProperty (compactId, compactView.load(), nullptr);
    state.setProperty (compactWId, compactWidth.load(), nullptr);
    state.setProperty (compactHId, compactHeight.load(), nullptr);
    state.appendChild (Subtitles::toValueTree (getSubtitles()), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void WhisperSubtitlesProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType (stateId))
        return;

    Transcriber::Settings s;
    s.language           = state.getProperty (languageId, s.language).toString();
    s.translateToEnglish = state.getProperty (translateId, s.translateToEnglish);
    s.maxCharsPerCue     = state.getProperty (maxCharsId, s.maxCharsPerCue);
    s.prompt             = state.getProperty (promptId, s.prompt).toString();
    s.dropNonSpeech      = state.getProperty (dropNonSpeechId, s.dropNonSpeech);
    setSettings (s);

    autoTranscribe = (bool) state.getProperty (autoId, true);
    capture.armed = (bool) state.getProperty (armedId, true);
    exportOffsetSeconds = (double) state.getProperty (offsetId, 0.0);
    compactView = (bool) state.getProperty (compactId, false);
    compactWidth = (int) state.getProperty (compactWId, 760);
    compactHeight = (int) state.getProperty (compactHId, 150);

    setSubtitles (Subtitles::fromValueTree (state.getChildWithName ("Subtitles")));

    const juce::File model (state.getProperty (modelId).toString());

    if (model != getModelFile() && model.existsAsFile())
        setModelFile (model);
}

//==============================================================================
juce::AudioProcessorEditor* WhisperSubtitlesProcessor::createEditor()
{
    return new WhisperSubtitlesEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WhisperSubtitlesProcessor();
}
