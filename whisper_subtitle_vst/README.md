# Whisper Subtitles (VST3 / AU / Standalone)

An audio plugin you put on your **dialogue track**. As the DAW plays, it listens to the track
and uses [Whisper](https://github.com/openai/whisper) (running locally through
[whisper.cpp](https://github.com/ggml-org/whisper.cpp)) to turn the dialogue into timed
subtitles, which you can then export as **.srt** or **.vtt**.

- Audio passes through untouched. The plugin only listens and adds no latency.
- Cue times come from the DAW timeline, so subtitles line up with your session and the picture.
- Everything runs on your machine. Nothing is uploaded.
- Subtitles and settings are saved with your DAW project.

## How to use it

1. **Insert** *Whisper Subtitles* on the dialogue track (or on a dialogue bus/stem).
2. Click **Load Whisper model...** and pick a `ggml-*.bin` file (see [Models](#models)).
3. Choose the **Language** (or leave it on Auto-detect). Add character names or jargon to
   **Names / terms** so Whisper spells them correctly.
4. **Press play.** The plugin records while the transport rolls (the label shows `● Recording`).
5. **Stop.** With *Auto-transcribe when playback stops* on, that stretch is transcribed in the
   background and the cues appear in the list. You can also press **Transcribe new audio** at any time.
6. The **Preview** box shows the cue at the playhead, so you can check timing as you play.
   Click **Subtitle view** to shrink the plugin into a black subtitle screen that shows only
   the current line as the playhead moves. Drag the window anywhere (e.g. under your video),
   resize it from the corner (the text scales with it), and click **Expand** or double-click it
   to get the full editor back. The plugin remembers the view and size with your project.
7. Fix any text directly in the SRT box, then click **Apply edits**.
8. Click **Export .srt...** or **Export .vtt...**. If your session starts at `01:00:00:00`, set
   **Export offset** to `3600` so the subtitle file starts at zero.

Tips
- Playing a passage again replaces the cues for that range, so you can redo sections after
  changing settings. **Re-transcribe all** redoes everything you've captured.
- Jumping the playhead or stopping starts a new capture region. You can play the timeline in
  any order.
- An offline bounce/export of the track also works as a capture pass.
- **Max chars / cue** splits on word boundaries (42 is a common one-line subtitle width).
  Set it to 0 to keep Whisper's natural sentence segments.
- **Translate to English** turns foreign-language dialogue into English subtitles (see below).
- Turn off **Capture while playing** to stop recording without removing the plugin.

## Malayalam, Bengali, Punjabi → English subtitles

1. Load a **multilingual** model. `ggml-large-v3.bin` gives the best results, and `ggml-medium.bin`
   is faster. English-only `.en` models can't translate. `large-v3-turbo` was trained without
   translation data, so it translates poorly. The plugin warns you if the loaded model doesn't fit.
2. Set **Language** to *Malayalam*, *Bengali* or *Punjabi*. Choosing the language beats
   Auto-detect on short lines and code-switched dialogue.
3. Tick **Translate to English**.
4. Play the track. The cues come out in English, timed to the original dialogue.

For a mixed-language film, put one instance on each language's dialogue track (or region) with
its own language set. Or use Auto-detect, which picks one language per captured stretch.
Write character names in **Names / terms** in English spelling so they come out consistently.

These languages have much less Whisper training data than English, so review the translated
subtitles before delivery. You can fix any line in the SRT box and click **Apply edits**.

## Models

Download a model with the included script (or grab any `ggml-*.bin` from
<https://huggingface.co/ggerganov/whisper.cpp>):

```bash
./scripts/download-model.sh large-v3     # for Malayalam/Bengali/Punjabi -> English
./scripts/download-model.sh base.en      # macOS / Linux, English dialogue
.\scripts\download-model.ps1 base.en     # Windows PowerShell
```

| Model | Size | Notes |
|---|---|---|
| `tiny.en` / `tiny` | 75 MB | fastest, rough |
| `base.en` / `base` | 142 MB | good default for clean dialogue |
| `small.en` / `small` | 466 MB | noticeably more accurate |
| `medium` | 1.5 GB | multilingual; decent translation |
| `large-v3` | 3.1 GB | best for Malayalam / Bengali / Punjabi and translation |
| `large-v3-turbo` | 1.6 GB | fast and multilingual, but **transcribe only**: poor at translating |

`.en` models are English-only and a bit more accurate for English. Use a model without `.en`
for other languages or for translation.

## Building

Requirements: CMake ≥ 3.22, a C++17 compiler (Xcode, Visual Studio 2022, or GCC/Clang), and git.
JUCE 8 and whisper.cpp are downloaded automatically during configure.

```bash
cd whisper_subtitle_vst
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

Output: `build/WhisperSubtitles_artefacts/Release/`
- `VST3/Whisper Subtitles.vst3`, copied to
  - macOS: `~/Library/Audio/Plug-Ins/VST3/`
  - Windows: `C:\Program Files\Common Files\VST3\`
  - Linux: `~/.vst3/`
- `AU/Whisper Subtitles.component` (macOS) → `~/Library/Audio/Plug-Ins/Components/`
- `Standalone/`: a standalone app that transcribes your audio input (handy for testing).

Options
- `-DWHISPER_NATIVE=ON` tunes whisper.cpp for *your* CPU (faster, but the binary is not portable).
- On Apple Silicon, whisper.cpp uses Metal automatically.
- Linux needs `libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
  libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`.

### Tests

```bash
cmake -B build -DWHISPER_SUBTITLES_TESTS=ON && cmake --build build --target WhisperSubtitlesTest
./build/WhisperSubtitlesTest_artefacts/Release/WhisperSubtitlesTest                       # capture + format tests
./build/WhisperSubtitlesTest_artefacts/Release/WhisperSubtitlesTest models/ggml-base.en.bin  # + real transcription
```

The test acts as a DAW: it drives a fake transport at 48 kHz and plays a speech clip at
`01:00:12.5`. It then checks capture, timeline alignment, playhead jumps, arming, session
save/restore and SRT/VTT output, and, if you pass a model, the actual transcription.

## How it works

```
DAW audio ──► processBlock ──(lock-free FIFO, mono)──► capture thread
                 │                                       low-pass + resample to 16 kHz
                 └─ playhead position                    split into regions on stop / jump
                                                                │
                                                 whisper.cpp worker thread
                                                 (beam search, word-level split)
                                                                │
                                   cues placed on the timeline ─► SRT / VTT / preview
```

The audio thread never locks or allocates. During an offline bounce it waits for the capture
thread instead of dropping audio.

Source layout: `Source/AudioCapture.*` (timeline-aligned capture), `Source/Transcriber.*`
(whisper.cpp), `Source/Subtitles.*` (SRT/VTT), `Source/PluginProcessor.*`, `Source/PluginEditor.*`.
