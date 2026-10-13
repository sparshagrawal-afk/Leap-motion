#include "Subtitles.h"

#include <algorithm>
#include <cmath>

namespace Subtitles
{

juce::String formatTimestamp (double seconds, char msSeparator)
{
    auto totalMs = (juce::int64) std::llround (juce::jmax (0.0, seconds) * 1000.0);
    auto ms = (int) (totalMs % 1000);
    auto totalSeconds = totalMs / 1000;
    auto s = (int) (totalSeconds % 60);
    auto m = (int) ((totalSeconds / 60) % 60);
    auto h = (int) (totalSeconds / 3600);

    return juce::String::formatted ("%02d:%02d:%02d", h, m, s)
         + juce::String::charToString ((juce::juce_wchar) msSeparator)
         + juce::String::formatted ("%03d", ms);
}

double parseTimestamp (const juce::String& text)
{
    auto t = text.trim().replaceCharacter (',', '.');
    auto parts = juce::StringArray::fromTokens (t, ":", "");

    if (parts.size() < 2 || parts.size() > 3)
        return -1.0;

    double result = 0.0;

    for (auto& p : parts)
    {
        if (p.isEmpty() || ! p.containsOnly ("0123456789."))
            return -1.0;

        result = result * 60.0 + p.getDoubleValue();
    }

    return result;
}

SubtitleList normalised (SubtitleList list, double offsetSeconds)
{
    list.erase (std::remove_if (list.begin(), list.end(),
                                [] (const SubtitleSegment& s) { return s.text.trim().isEmpty(); }),
                list.end());

    std::stable_sort (list.begin(), list.end(),
                      [] (const SubtitleSegment& a, const SubtitleSegment& b) { return a.start < b.start; });

    for (size_t i = 0; i < list.size(); ++i)
    {
        auto& s = list[i];
        s.start = juce::jmax (0.0, s.start - offsetSeconds);
        s.end   = juce::jmax (s.start + 0.2, s.end - offsetSeconds);
        s.text  = s.text.trim();

        if (i > 0 && list[i - 1].end > s.start)
            list[i - 1].end = juce::jmax (list[i - 1].start + 0.05, s.start);
    }

    return list;
}

juce::String toSrt (const SubtitleList& input, double offsetSeconds)
{
    juce::String out;
    int index = 1;

    for (auto& s : normalised (input, offsetSeconds))
    {
        out << index++ << "\n"
            << formatTimestamp (s.start, ',') << " --> " << formatTimestamp (s.end, ',') << "\n"
            << s.text << "\n\n";
    }

    return out;
}

juce::String toVtt (const SubtitleList& input, double offsetSeconds)
{
    juce::String out ("WEBVTT\n\n");

    for (auto& s : normalised (input, offsetSeconds))
    {
        out << formatTimestamp (s.start, '.') << " --> " << formatTimestamp (s.end, '.') << "\n"
            << s.text << "\n\n";
    }

    return out;
}

SubtitleList fromSrt (const juce::String& text)
{
    SubtitleList result;

    auto lines = juce::StringArray::fromLines (text.replace ("\r\n", "\n"));
    lines.add ({}); // guarantees the last block is flushed

    SubtitleSegment current;
    bool haveTiming = false;
    juce::StringArray body;

    auto flush = [&]
    {
        if (haveTiming && body.size() > 0)
        {
            current.text = body.joinIntoString ("\n").trim();
            result.push_back (current);
        }

        haveTiming = false;
        body.clear();
    };

    for (auto& rawLine : lines)
    {
        auto line = rawLine.trimEnd();

        if (line.trim().isEmpty())
        {
            flush();
            continue;
        }

        if (line.contains ("-->"))
        {
            flush();

            auto startText = line.upToFirstOccurrenceOf ("-->", false, false);
            auto endText   = line.fromFirstOccurrenceOf ("-->", false, false).trim()
                                 .upToFirstOccurrenceOf (" ", false, false); // drop VTT cue settings

            auto start = parseTimestamp (startText);
            auto end   = parseTimestamp (endText);

            if (start >= 0.0 && end >= 0.0)
            {
                current = {};
                current.start = start;
                current.end = juce::jmax (start, end);
                haveTiming = true;
            }

            continue;
        }

        if (haveTiming)
            body.add (line);
        // lines before a timing line are cue numbers / headers and are ignored
    }

    return result;
}

void replaceRange (SubtitleList& target, double rangeStart, double rangeEnd, const SubtitleList& replacement)
{
    target.erase (std::remove_if (target.begin(), target.end(),
                                  [&] (const SubtitleSegment& s)
                                  {
                                      return s.end > rangeStart && s.start < rangeEnd;
                                  }),
                  target.end());

    target.insert (target.end(), replacement.begin(), replacement.end());

    std::stable_sort (target.begin(), target.end(),
                      [] (const SubtitleSegment& a, const SubtitleSegment& b) { return a.start < b.start; });
}

SubtitleList mapSourceToTimeline (const SubtitleList& sourceCues,
                                  double sourceStart, double sourceEnd,
                                  double timelineStart, double timelineEnd)
{
    SubtitleList result;

    const double sourceLength = sourceEnd - sourceStart;
    if (sourceLength <= 0.0)
        return result;

    const double scale = (timelineEnd - timelineStart) / sourceLength;

    for (auto& cue : sourceCues)
    {
        const double s = juce::jmax (cue.start, sourceStart);
        const double e = juce::jmin (cue.end, sourceEnd);

        if (e - s < 0.05) // outside the clip, or only a sliver of it left
            continue;

        result.push_back ({ timelineStart + (s - sourceStart) * scale,
                            timelineStart + (e - sourceStart) * scale,
                            cue.text });
    }

    return result;
}

const SubtitleSegment* findAt (const SubtitleList& list, double seconds)
{
    for (auto& s : list)
        if (seconds >= s.start && seconds < s.end)
            return &s;

    return nullptr;
}

static const juce::Identifier subtitlesId ("Subtitles"), cueId ("Cue"),
                              startId ("start"), endId ("end"), textId ("text");

juce::ValueTree toValueTree (const SubtitleList& list)
{
    juce::ValueTree tree (subtitlesId);

    for (auto& s : list)
    {
        juce::ValueTree cue (cueId);
        cue.setProperty (startId, s.start, nullptr);
        cue.setProperty (endId, s.end, nullptr);
        cue.setProperty (textId, s.text, nullptr);
        tree.appendChild (cue, nullptr);
    }

    return tree;
}

SubtitleList fromValueTree (const juce::ValueTree& tree)
{
    SubtitleList list;

    for (auto cue : tree)
    {
        if (! cue.hasType (cueId))
            continue;

        list.push_back ({ (double) cue.getProperty (startId),
                          (double) cue.getProperty (endId),
                          cue.getProperty (textId).toString() });
    }

    return list;
}

} // namespace Subtitles
