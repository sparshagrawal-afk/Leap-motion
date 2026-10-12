#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <vector>

/** One subtitle cue. Times are seconds on the host timeline. */
struct SubtitleSegment
{
    double start = 0.0;
    double end = 0.0;
    juce::String text;
};

using SubtitleList = std::vector<SubtitleSegment>;

namespace Subtitles
{
    /** "HH:MM:SS,mmm" (SRT) or "HH:MM:SS.mmm" (VTT). */
    juce::String formatTimestamp (double seconds, char msSeparator);

    /** Parses "HH:MM:SS,mmm", "HH:MM:SS.mmm" or "MM:SS.mmm". Returns -1 on failure. */
    double parseTimestamp (const juce::String& text);

    /** Sorts by start time and trims overlaps so every cue ends before the next one starts.
        @param offsetSeconds  subtracted from every time (e.g. to make 01:00:00:00 start at zero). */
    SubtitleList normalised (SubtitleList list, double offsetSeconds = 0.0);

    juce::String toSrt (const SubtitleList&, double offsetSeconds = 0.0);
    juce::String toVtt (const SubtitleList&, double offsetSeconds = 0.0);

    /** Parses SRT (or VTT) text back into cues. Malformed blocks are skipped. */
    SubtitleList fromSrt (const juce::String& text);

    /** Replaces every cue that overlaps [rangeStart, rangeEnd] with the new cues. */
    void replaceRange (SubtitleList& target, double rangeStart, double rangeEnd, const SubtitleList& replacement);

    /** Returns the cue showing at the given time, or nullptr. */
    const SubtitleSegment* findAt (const SubtitleList&, double seconds);

    juce::ValueTree toValueTree (const SubtitleList&);
    SubtitleList fromValueTree (const juce::ValueTree&);
}
