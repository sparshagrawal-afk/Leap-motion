#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class WhisperSubtitlesEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit WhisperSubtitlesEditor (WhisperSubtitlesProcessor&);
    ~WhisperSubtitlesEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void chooseModel();
    void pushSettings();
    void refreshSubtitleText();
    void applyEdits();
    void exportFile (bool vtt);
    void updateEditButtons();

    WhisperSubtitlesProcessor& audioProcessor;

    juce::TextButton loadModelButton { "Load Whisper model..." };
    juce::Label modelLabel;

    juce::Label languageLabel { {}, "Language" };
    juce::ComboBox languageBox;
    juce::ToggleButton translateToggle { "Translate to English" };
    juce::Label maxCharsLabel { {}, "Max chars / cue" };
    juce::Slider maxCharsSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ToggleButton dropNonSpeechToggle { "Drop [music] / (noise) cues" };

    juce::Label promptLabel { {}, "Names / terms" };
    juce::TextEditor promptEditor;

    juce::ToggleButton armedToggle { "Capture while playing" };
    juce::ToggleButton autoToggle { "Auto-transcribe when playback stops" };
    juce::TextButton transcribeButton { "Transcribe new audio" };
    juce::TextButton retranscribeButton { "Re-transcribe all" };
    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton clearAudioButton { "Clear audio" };
    juce::TextButton clearSubsButton { "Clear subtitles" };

    juce::Label captureLabel, statusLabel;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    juce::String previewText;
    juce::Rectangle<int> previewArea;

    juce::TextEditor srtEditor;
    juce::TextButton applyButton { "Apply edits" };
    juce::TextButton revertButton { "Revert" };
    juce::Label offsetLabel { {}, "Export offset (s)" };
    juce::TextEditor offsetEditor;
    juce::TextButton exportSrtButton { "Export .srt..." };
    juce::TextButton exportVttButton { "Export .vtt..." };
    juce::TextButton copyButton { "Copy SRT" };

    std::unique_ptr<juce::FileChooser> fileChooser;
    int shownVersion = -1;
    bool editsPending = false;
    bool settingTextProgrammatically = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WhisperSubtitlesEditor)
};
