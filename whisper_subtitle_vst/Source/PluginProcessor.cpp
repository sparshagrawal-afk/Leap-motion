#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier stateId ("WhisperSubtitlesState"),
                           modelId ("modelPath"), languageId ("language"), translateId ("translate"),
                           maxCharsId ("maxChars"), promptId ("prompt"), dropNonSpeechId ("dropNonSpeech"),
                           autoId ("autoTranscribe"), armedId ("armed"), offsetId ("exportOffset");
}

WhisperSubtitlesProcessor::WhisperSubtitlesProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    capture.onRegionFinished = [this] (const AudioCapture::Region& r) { handleRegionFinished (r); };
    transcriber.onResult = [this] (double s, double e, SubtitleList cues) { handleTranscription (s, e, std::move (cues)); };
}

WhisperSubtitlesProcessor::~WhisperSubtitlesProcessor()
{
    // Stop both worker threads before the members their callbacks touch are destroyed.
    capture.shutdown();
    transcriber.shutdown();
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

    transcriber.loadModel (f);
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
        transcriber.transcribe (region.startSeconds, region.samples, getSettings());
}

void WhisperSubtitlesProcessor::transcribeCaptured (bool all)
{
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
            transcriber.transcribe (region.startSeconds, region.samples, s);
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
    {
        const juce::ScopedLock sl (subtitleLock);
        subtitles.clear();
        transcribedRanges.clear();
    }

    ++subtitleVersion;
}

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
