#pragma once
// AutoDelay Pro: delay styles, note values and THE Lite/Pro split.
// Everything about what is free and what needs Pro lives in this file. To move a feature,
// flip its flag below: the processor (audio), the editor (padlocks) and the tests all read it.

namespace ad
{
enum Style { clean = 0, pingPong, slapback, tape, dub, analog, lofi, numStyles };

inline const char* styleName (int s)
{
    static const char* n[] = { "CLEAN", "PING PONG", "SLAPBACK", "TAPE", "DUB", "ANALOG", "LO-FI" };
    return n[s >= 0 && s < numStyles ? s : 0];
}

// Note values (in beats). The last TIME position is AUTO (the style picks).
inline constexpr int numNotes = 14, timeAuto = 14;
inline constexpr double noteBeats[numNotes] = { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75,
                                                2.0 / 3.0, 1.0, 1.5, 4.0 / 3.0, 2.0, 3.0, 4.0 };
inline const char* noteName (int i)
{
    static const char* n[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D",
                               "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1/1" };
    return i >= 0 && i < numNotes ? n[i] : "AUTO";
}
enum NoteIdx { n32 = 0, n16T, n16, n16D, n8T, n8, n8D, n4T, n4, n4D, n2T, n2, n2D, n1 };

// AUTO: the note value each style picks by itself (the user never does the maths).
inline int autoNote (int style, double bpm)
{
    switch (style)
    {
        case pingPong: return n8;
        case slapback: return bpm >= 118.0 ? n16 : n32;   // whichever is nearest 90 ms: always a slap (64-127 ms from 60 to 236 BPM)
        case tape:     return n8D;
        case dub:      return n4D;
        case analog:   return n4;
        case lofi:     return n8;
        default:       return n8D;                         // CLEAN
    }
}

// Default positions of the continuous controls (Lite runs these values).
inline constexpr float defFeedback = 40.0f, defMix = 30.0f, defDuck = 60.0f, defTone = 0.0f,
                       defDrive = 0.0f, defWidth = 100.0f;

// ── THE LITE / PRO SPLIT ──────────────────────────────────────────────────────
// true = needs Pro (licence key or 7-day trial). false = free in Lite.
// Always free: AUTO time from the host tempo, FEEDBACK, MIX, AUTO DUCKING (fixed amount), the display.
namespace gate
{
    inline constexpr bool proStyle[numStyles] = {
        false,  // CLEAN
        false,  // PING PONG
        true,   // SLAPBACK
        true,   // TAPE
        true,   // DUB
        true,   // ANALOG
        true    // LO-FI
    };
    inline constexpr bool proTime   = true;   // Lite: AUTO (the style's note value)
    inline constexpr bool proDuck   = true;   // Lite: ducking on at defDuck
    inline constexpr bool proThrows = true;   // Lite: off (every note feeds the delay)
    inline constexpr bool proTone   = true;   // Lite: 0 (the style's own tone)
    inline constexpr bool proDrive  = true;   // Lite: 0
    inline constexpr bool proWidth  = true;   // Lite: 100 %
    inline constexpr bool proFreeze = true;   // Lite: off

    inline bool styleAllowed (int s, bool pro) { return pro || ! proStyle[s >= 0 && s < numStyles ? s : 0]; }
    // What a locked style falls back to in Lite.
    inline int  effectiveStyle (int s, bool pro) { return styleAllowed (s, pro) ? s : (int) clean; }
}
} // namespace ad
