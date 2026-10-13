#include "PluginEditor.h"

namespace
{
    struct Language { const char* code; const char* name; };

    const Language languages[] = {
        { "auto", "Auto-detect" }, { "en", "English" },
        // Indian languages first: Whisper can transcribe these and translate them to English
        { "ml", "Malayalam" }, { "bn", "Bengali" }, { "pa", "Punjabi" }, { "hi", "Hindi" },
        { "ta", "Tamil" }, { "te", "Telugu" }, { "kn", "Kannada" }, { "mr", "Marathi" },
        { "gu", "Gujarati" }, { "ur", "Urdu" }, { "ne", "Nepali" }, { "si", "Sinhala" },
        { "es", "Spanish" }, { "fr", "French" }, { "de", "German" }, { "it", "Italian" },
        { "pt", "Portuguese" }, { "nl", "Dutch" }, { "ru", "Russian" }, { "uk", "Ukrainian" },
        { "pl", "Polish" }, { "tr", "Turkish" }, { "ar", "Arabic" }, { "he", "Hebrew" },
        { "zh", "Chinese" }, { "ja", "Japanese" }, { "ko", "Korean" }, { "sv", "Swedish" },
        { "da", "Danish" }, { "no", "Norwegian" }, { "fi", "Finnish" }, { "el", "Greek" },
        { "cs", "Czech" }, { "ro", "Romanian" }, { "hu", "Hungarian" }, { "id", "Indonesian" },
        { "vi", "Vietnamese" }, { "th", "Thai" },
    };

    /** Explains when the loaded model can't do what the settings ask for. */
    juce::String modelWarning (const juce::String& modelName, const Transcriber::Settings& s)
    {
        const auto name = modelName.toLowerCase();
        const bool englishOnly = name.contains (".en.") || name.endsWith (".en.bin");
        const bool nonEnglish = s.language != "en" && s.language != "auto";

        if (englishOnly && (s.translateToEnglish || nonEnglish))
            return "English-only model: load a multilingual one (e.g. ggml-large-v3.bin) to translate";

        if (s.translateToEnglish && name.contains ("turbo"))
            return "turbo models translate poorly: use ggml-large-v3.bin or ggml-medium.bin";

        return {};
    }

    const juce::Colour background (0xff1b1d22), panel (0xff252830), accent (0xff4fc3f7);
}

WhisperSubtitlesEditor::WhisperSubtitlesEditor (WhisperSubtitlesProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    const auto settings = audioProcessor.getSettings();

    // --- model
    addAndMakeVisible (loadModelButton);
    loadModelButton.onClick = [this] { chooseModel(); };
    addAndMakeVisible (modelLabel);
    modelLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    // --- recognition settings
    addAndMakeVisible (languageLabel);
    addAndMakeVisible (languageBox);
    int id = 1, selected = 1;
    for (auto& lang : languages)
    {
        if (settings.language == lang.code)
            selected = id;

        languageBox.addItem (lang.name, id++);
    }
    languageBox.setSelectedId (selected, juce::dontSendNotification);
    languageBox.onChange = [this] { pushSettings(); };

    addAndMakeVisible (translateToggle);
    translateToggle.setToggleState (settings.translateToEnglish, juce::dontSendNotification);
    translateToggle.onClick = [this] { pushSettings(); };
    translateToggle.setTooltip ("Subtitles in English, whatever language is spoken (e.g. Malayalam, Bengali, Punjabi). "
                                "Needs a multilingual model such as ggml-large-v3.bin.");

    addAndMakeVisible (maxCharsLabel);
    addAndMakeVisible (maxCharsSlider);
    maxCharsSlider.setRange (0, 120, 1);
    maxCharsSlider.setValue (settings.maxCharsPerCue, juce::dontSendNotification);
    maxCharsSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 24);
    maxCharsSlider.setTooltip ("0 = natural Whisper segments; 42 is a common single-line subtitle width");
    maxCharsSlider.onValueChange = [this] { pushSettings(); };

    addAndMakeVisible (dropNonSpeechToggle);
    dropNonSpeechToggle.setToggleState (settings.dropNonSpeech, juce::dontSendNotification);
    dropNonSpeechToggle.onClick = [this] { pushSettings(); };

    addAndMakeVisible (promptLabel);
    addAndMakeVisible (promptEditor);
    promptEditor.setText (settings.prompt, false);
    promptEditor.setTextToShowWhenEmpty ("Character names, places, jargon - helps Whisper spell them right",
                                         juce::Colours::grey);
    promptEditor.onTextChange = [this] { pushSettings(); };

    // --- capture / transcription
    addAndMakeVisible (armedToggle);
    armedToggle.setToggleState (audioProcessor.capture.armed.load(), juce::dontSendNotification);
    armedToggle.onClick = [this] { audioProcessor.capture.armed = armedToggle.getToggleState(); };

    addAndMakeVisible (autoToggle);
    autoToggle.setToggleState (audioProcessor.autoTranscribe.load(), juce::dontSendNotification);
    autoToggle.onClick = [this] { audioProcessor.autoTranscribe = autoToggle.getToggleState(); };

    addAndMakeVisible (transcribeButton);
    transcribeButton.onClick = [this] { audioProcessor.transcribeCaptured (false); };
    addAndMakeVisible (retranscribeButton);
    retranscribeButton.onClick = [this] { audioProcessor.transcribeCaptured (true); };
    addAndMakeVisible (cancelButton);
    cancelButton.onClick = [this] { audioProcessor.transcriber.cancelAll(); };
    addAndMakeVisible (clearAudioButton);
    clearAudioButton.onClick = [this] { audioProcessor.capture.clear(); };
    addAndMakeVisible (clearSubsButton);
    clearSubsButton.onClick = [this]
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Clear subtitles",
                                            "Delete every subtitle cue?", "Clear", "Keep", this,
                                            juce::ModalCallbackFunction::create ([this] (int result)
                                            {
                                                if (result != 0)
                                                    audioProcessor.clearSubtitles();
                                            }));
    };

    addAndMakeVisible (captureLabel);
    addAndMakeVisible (statusLabel);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (progressBar);
    progressBar.setColour (juce::ProgressBar::foregroundColourId, accent);

    // --- subtitle text
    addAndMakeVisible (srtEditor);
    srtEditor.setMultiLine (true, false);
    srtEditor.setReturnKeyStartsNewLine (true);
    srtEditor.setScrollbarsShown (true);
    srtEditor.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
    srtEditor.setTextToShowWhenEmpty ("Subtitles appear here. Play the dialogue through the plugin, "
                                      "then stop playback (or press Transcribe).", juce::Colours::grey);
    srtEditor.onTextChange = [this]
    {
        if (! settingTextProgrammatically)
        {
            editsPending = true;
            updateEditButtons();
        }
    };

    addAndMakeVisible (applyButton);
    applyButton.onClick = [this] { applyEdits(); };
    addAndMakeVisible (revertButton);
    revertButton.onClick = [this] { editsPending = false; refreshSubtitleText(); };

    addAndMakeVisible (offsetLabel);
    addAndMakeVisible (offsetEditor);
    offsetEditor.setInputRestrictions (12, "0123456789.-");
    offsetEditor.setText (juce::String (audioProcessor.exportOffsetSeconds.load()), false);
    offsetEditor.setTooltip ("Subtracted from every cue on export. Use 3600 if your session starts at 01:00:00:00.");
    offsetEditor.onTextChange = [this] { audioProcessor.exportOffsetSeconds = offsetEditor.getText().getDoubleValue(); };

    addAndMakeVisible (exportSrtButton);
    exportSrtButton.onClick = [this] { exportFile (false); };
    addAndMakeVisible (exportVttButton);
    exportVttButton.onClick = [this] { exportFile (true); };
    addAndMakeVisible (copyButton);
    copyButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (Subtitles::toSrt (audioProcessor.getSubtitles(),
                                                                      audioProcessor.exportOffsetSeconds.load()));
    };

    setResizable (true, true);
    setResizeLimits (760, 560, 2000, 1600);
    setSize (920, 660);

    updateEditButtons();
    timerCallback();
    startTimerHz (15);
}

WhisperSubtitlesEditor::~WhisperSubtitlesEditor()
{
    stopTimer();
}

//==============================================================================
void WhisperSubtitlesEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().reduced (8).withHeight (180).toFloat(), 6.0f);

    // Subtitle preview, like it would sit on the picture
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (previewArea.toFloat(), 6.0f);

    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Preview @ " + Subtitles::formatTimestamp (audioProcessor.getPlayheadSeconds(), '.'),
                previewArea.reduced (8, 4), juce::Justification::topLeft);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawFittedText (previewText, previewArea.reduced (16, 18), juce::Justification::centredBottom, 2);
}

void WhisperSubtitlesEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    const int rowH = 26, gap = 8;

    auto row = area.removeFromTop (rowH);
    loadModelButton.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (gap);
    modelLabel.setBounds (row);
    area.removeFromTop (gap);

    row = area.removeFromTop (rowH);
    languageLabel.setBounds (row.removeFromLeft (70));
    languageBox.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (gap * 2);
    translateToggle.setBounds (row.removeFromLeft (170));
    maxCharsLabel.setBounds (row.removeFromLeft (110));
    maxCharsSlider.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (gap * 2);
    dropNonSpeechToggle.setBounds (row);
    area.removeFromTop (gap);

    row = area.removeFromTop (rowH);
    promptLabel.setBounds (row.removeFromLeft (100));
    promptEditor.setBounds (row);
    area.removeFromTop (gap);

    row = area.removeFromTop (rowH);
    armedToggle.setBounds (row.removeFromLeft (180));
    autoToggle.setBounds (row.removeFromLeft (270));
    clearSubsButton.setBounds (row.removeFromRight (110));
    row.removeFromRight (gap);
    clearAudioButton.setBounds (row.removeFromRight (95));
    area.removeFromTop (gap);

    row = area.removeFromTop (rowH);
    transcribeButton.setBounds (row.removeFromLeft (160));
    row.removeFromLeft (gap);
    retranscribeButton.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (gap);
    cancelButton.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (gap * 2);
    progressBar.setBounds (row);
    area.removeFromTop (gap * 2);

    row = area.removeFromTop (20);
    captureLabel.setBounds (row.removeFromLeft (row.getWidth() / 2));
    statusLabel.setBounds (row);
    area.removeFromTop (gap);

    previewArea = area.removeFromTop (100);
    area.removeFromTop (gap);

    auto bottom = area.removeFromBottom (rowH);
    applyButton.setBounds (bottom.removeFromLeft (100));
    bottom.removeFromLeft (gap);
    revertButton.setBounds (bottom.removeFromLeft (70));
    copyButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (gap);
    exportVttButton.setBounds (bottom.removeFromRight (105));
    bottom.removeFromRight (gap);
    exportSrtButton.setBounds (bottom.removeFromRight (105));
    bottom.removeFromRight (gap * 2);
    offsetEditor.setBounds (bottom.removeFromRight (70));
    offsetLabel.setBounds (bottom.removeFromRight (120));
    area.removeFromBottom (gap);

    srtEditor.setBounds (area);
}

//==============================================================================
void WhisperSubtitlesEditor::timerCallback()
{
    auto& t = audioProcessor.transcriber;
    auto& c = audioProcessor.capture;

    const auto model = audioProcessor.getModelFile();
    const auto warning = modelWarning (model.getFileName(), audioProcessor.getSettings());
    modelLabel.setText (t.isModelLoaded() ? "Model: " + model.getFileName() + (warning.isEmpty() ? "" : "  -  " + warning)
                                          : (model == juce::File() ? "No model loaded - pick a ggml-*.bin file"
                                                                   : t.getStatus()),
                        juce::dontSendNotification);
    modelLabel.setColour (juce::Label::textColourId, t.isModelLoaded() && warning.isEmpty() ? juce::Colours::lightgreen
                                                                                            : juce::Colours::orange);

    juce::String captureText;
    captureText << (c.isRecording() ? juce::String::charToString (0x25cf) + " Recording   " : juce::String())
                << "Captured " << juce::String (c.getCapturedSeconds(), 1) << " s";
    if (c.getDroppedBlocks() > 0)
        captureText << "  (" << c.getDroppedBlocks() << " blocks dropped)";
    captureLabel.setText (captureText, juce::dontSendNotification);
    captureLabel.setColour (juce::Label::textColourId, c.isRecording() ? juce::Colours::tomato
                                                                       : juce::Colours::lightgrey);

    juce::String status = t.getStatus();
    if (t.getPendingJobs() > 1)
        status << "  (+" << (t.getPendingJobs() - 1) << " queued)";
    statusLabel.setText (status, juce::dontSendNotification);

    progressValue = t.isBusy() ? (double) t.getProgress() : 0.0;
    transcribeButton.setEnabled (t.isModelLoaded());
    retranscribeButton.setEnabled (t.isModelLoaded());
    cancelButton.setEnabled (t.isBusy());

    if (audioProcessor.getSubtitleVersion() != shownVersion && ! editsPending)
        refreshSubtitleText();

    const auto subs = audioProcessor.getSubtitles();
    juce::String now;
    if (auto* cue = Subtitles::findAt (subs, audioProcessor.getPlayheadSeconds()))
        now = cue->text;

    if (now != previewText || audioProcessor.isHostPlaying())
    {
        previewText = now;
        repaint (previewArea);
    }
}

void WhisperSubtitlesEditor::refreshSubtitleText()
{
    shownVersion = audioProcessor.getSubtitleVersion();

    const juce::ScopedValueSetter<bool> svs (settingTextProgrammatically, true);
    srtEditor.setText (Subtitles::toSrt (audioProcessor.getSubtitles()), false);
    updateEditButtons();
}

void WhisperSubtitlesEditor::applyEdits()
{
    audioProcessor.setSubtitles (Subtitles::fromSrt (srtEditor.getText()));
    editsPending = false;
    refreshSubtitleText();
}

void WhisperSubtitlesEditor::updateEditButtons()
{
    applyButton.setEnabled (editsPending);
    revertButton.setEnabled (editsPending);
}

void WhisperSubtitlesEditor::pushSettings()
{
    Transcriber::Settings s;

    const int index = languageBox.getSelectedId() - 1;
    if (juce::isPositiveAndBelow (index, (int) std::size (languages)))
        s.language = languages[index].code;

    s.translateToEnglish = translateToggle.getToggleState();
    s.maxCharsPerCue = (int) maxCharsSlider.getValue();
    s.prompt = promptEditor.getText();
    s.dropNonSpeech = dropNonSpeechToggle.getToggleState();

    audioProcessor.setSettings (s);
}

//==============================================================================
void WhisperSubtitlesEditor::chooseModel()
{
    auto start = audioProcessor.getModelFile();
    if (! start.existsAsFile())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    fileChooser = std::make_unique<juce::FileChooser> ("Choose a Whisper ggml model", start, "*.bin");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      audioProcessor.setModelFile (f);
                              });
}

void WhisperSubtitlesEditor::exportFile (bool vtt)
{
    if (editsPending)
        applyEdits();

    const auto ext = vtt ? juce::String (".vtt") : juce::String (".srt");
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("dialogue" + ext);

    fileChooser = std::make_unique<juce::FileChooser> ("Export subtitles", start, "*" + ext);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, vtt, ext] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f == juce::File())
                                      return;

                                  f = f.withFileExtension (ext);
                                  const auto subs = audioProcessor.getSubtitles();
                                  const auto offset = audioProcessor.exportOffsetSeconds.load();
                                  const auto text = vtt ? Subtitles::toVtt (subs, offset)
                                                        : Subtitles::toSrt (subs, offset);

                                  if (! f.replaceWithText (text, false, false, "\n"))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                              "Export failed",
                                                                              "Could not write " + f.getFullPathName());
                              });
}
