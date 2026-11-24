#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

static constexpr double maxDelaySecs = 6.0;    // longer note values are halved until they fit (1/1 below 40 BPM)
static constexpr double histSecs     = 3.0;    // input history for throws
static constexpr double gapSecs      = 0.085;  // a dip this long ends a phrase (gaps between sung words are shorter)
static constexpr double refRelSecs   = 0.5;    // the "recent level" falls ~17 dB/s: faster than a pad fades, slower than a word stops
static constexpr float  wordRatio    = 0.6f;   // a word is sounding while the level is within ~4.4 dB of the recent level
static constexpr float  deepRatio    = 0.45f;  // ...and a phrase-ending dip reaches ~7 dB under it (reverb may fill the rest)
static constexpr float  onsetRatio   = 1.6f;   // a phrase starts on a rise of 4 dB+ over the last ~40 ms
static constexpr double minPhrase    = 0.25;   // a phrase must last this long to get a throw
static constexpr double maxTailSecs  = 0.8;    // longest "last word" a throw catches
static constexpr double preRollSecs  = 0.015;

AutoDelayProAudioProcessor::AutoDelayProAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AutoDelayProState", createParameterLayout())
{
    pStyle    = apvts.getRawParameterValue ("style");
    pTime     = apvts.getRawParameterValue ("time");
    pFeedback = apvts.getRawParameterValue ("feedback");
    pMix      = apvts.getRawParameterValue ("mix");
    pDuck     = apvts.getRawParameterValue ("duck");
    pThrows   = apvts.getRawParameterValue ("throws");
    pTone     = apvts.getRawParameterValue ("tone");
    pDrive    = apvts.getRawParameterValue ("drive");
    pWidth    = apvts.getRawParameterValue ("width");
    pFreeze   = apvts.getRawParameterValue ("freeze");
    setLicenceInfo (licence.check());
}

juce::AudioProcessorValueTreeState::ParameterLayout AutoDelayProAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto id = [] (const char* s) { return ParameterID { s, 1 }; };
    auto pct = [] { return AudioParameterFloatAttributes().withLabel ("%"); };

    l.add (std::make_unique<AudioParameterChoice> (id ("style"), "Style",
               StringArray { "Clean", "Ping Pong", "Slapback", "Tape", "Dub", "Analog", "Lo-Fi" }, 0));
    StringArray notes; for (int i = 0; i < ad::numNotes; ++i) notes.add (ad::noteName (i)); notes.add ("Auto");
    l.add (std::make_unique<AudioParameterChoice> (id ("time"), "Time", notes, ad::timeAuto));
    l.add (std::make_unique<AudioParameterFloat> (id ("feedback"), "Feedback", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ad::defFeedback, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("mix"), "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ad::defMix, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("duck"), "Ducking", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ad::defDuck, pct()));
    l.add (std::make_unique<AudioParameterBool> (id ("throws"), "Auto Throws", false));
    l.add (std::make_unique<AudioParameterFloat> (id ("tone"), "Tone", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), ad::defTone,
               AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
               { return std::abs (v) < 0.5f ? String ("0") : (v < 0 ? "Dark " + String (-v, 0) : "Bright " + String (v, 0)); })));
    l.add (std::make_unique<AudioParameterFloat> (id ("drive"), "Drive", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ad::defDrive, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("width"), "Width", NormalisableRange<float> (0.0f, 150.0f, 0.1f), ad::defWidth, pct()));
    l.add (std::make_unique<AudioParameterBool> (id ("freeze"), "Freeze", false));
    return l;
}

// ── Resolve: Lite/Pro gate + AUTO -> what actually runs ───────────────────────
AutoDelayProAudioProcessor::Effective AutoDelayProAudioProcessor::resolve (bool pro, double bpm) const
{
    using namespace ad::gate;
    Effective e;
    e.style = effectiveStyle (juce::jlimit (0, ad::numStyles - 1, (int) pStyle->load()), pro);
    const int t = (pro || ! proTime) ? (int) pTime->load() : ad::timeAuto;
    e.timeAuto = t < 0 || t >= ad::numNotes;
    e.note     = e.timeAuto ? ad::autoNote (e.style, bpm) : t;
    e.feedback = juce::jlimit (0.0f, 1.0f, pFeedback->load() / 100.0f);
    e.mix      = juce::jlimit (0.0f, 1.0f, pMix->load() / 100.0f);
    e.duck     = juce::jlimit (0.0f, 1.0f, ((pro || ! proDuck)  ? pDuck->load()  : ad::defDuck)  / 100.0f);
    e.tone     = juce::jlimit (-1.0f, 1.0f, ((pro || ! proTone)  ? pTone->load()  : ad::defTone)  / 100.0f);
    e.drive    = juce::jlimit (0.0f, 1.0f, ((pro || ! proDrive) ? pDrive->load() : ad::defDrive) / 100.0f);
    e.width    = juce::jlimit (0.0f, 1.5f, ((pro || ! proWidth) ? pWidth->load() : ad::defWidth) / 100.0f);
    e.throws   = (pro || ! proThrows) && pThrows->load() > 0.5f;
    e.freeze   = (pro || ! proFreeze) && pFreeze->load() > 0.5f;
    return e;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void AutoDelayProAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    size = (int) std::ceil ((maxDelaySecs + 0.5) * sr) + 8;
    for (int c = 0; c < 2; ++c) { line[c].assign ((size_t) size, 0.0f); inject[c].assign ((size_t) size, 0.0f); }
    w = 0;
    histSize = (int) std::ceil (histSecs * sr);
    for (auto& h : hist) h.assign ((size_t) histSize, 0.0f);

    cFast = (float) (1.0 - std::exp (-1.0 / (0.02 * sr)));
    xfStep = (float) (1.0 / (0.05 * sr));
    glideCoef = 1.0 - std::exp (-1.0 / (0.25 * sr));
    smallGlideCoef = 1.0 - std::exp (-1.0 / (0.03 * sr));
    duckEnvA = (float) (1.0 - std::exp (-1.0 / (0.005 * sr)));
    duckEnvR = (float) (1.0 - std::exp (-1.0 / (0.08 * sr)));
    duckAtk  = (float) (1.0 - std::exp (-1.0 / (0.012 * sr)));
    duckRel  = (float) (1.0 - std::exp (-1.0 / (0.30 * sr)));
    peakRel  = (float) std::exp (-1.0 / (3.0 * sr));
    phRefRel = (float) std::exp (-1.0 / (refRelSecs * sr));
    phMsC    = (float) (1.0 - std::exp (-1.0 / (0.01 * sr)));
    phSlowC  = (float) (1.0 - std::exp (-1.0 / (0.04 * sr)));
    for (int c = 0; c < 2; ++c) { lpf[c].reset(); hpf[c].reset(); decor[c].reset(); }
    decor[0].a = 0.62f; decor[1].a = -0.35f;
    wowPh = flutPh = 0.0;
    duckEnv = duckGainDb = 0.0f; peakTrack = 1.0e-4f;
    phRef = phSlow = phMs = 0; inPhrase = wordOn = dipDeep = false;
    onFast = onSlow = 0;
    clock.store (0); lastHit.store (-1); lastOnset = -100000;
    lastStyle = -1; fading = false; xf = 1.0f; lastLpT = lastHpT = -1.0f;
    prepared = false;   // first block snaps everything to the current settings (no ramps on load)
}

bool AutoDelayProAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

// Targets for the smoothers + the delay time (samples) from the note value and tempo.
void AutoDelayProAudioProcessor::updateTargets (const Effective& e, double bpm, bool snap)
{
    const auto& m = ad::model (e.style);
    const float lp = ad::lpFor (e.style, e.tone, e.drive), hp = ad::hpFor (e.style, e.tone, e.drive);
    // Feedback never runs away: the loop gain is scaled so even the filters' resonant peak stays under 1.
    if (lp != lastLpT || hp != lastHpT)
    {
        lastLpT = lp; lastHpT = hp; maxMag = 1.0;
        for (double f = 20.0; f < 0.45 * sr; f *= 1.03) maxMag = juce::jmax (maxMag, ad::loopMag (lp, m.lpQ, hp, m.hpQ, f));
    }
    sFb.t = (float) (ad::loopGain (e.feedback, e.style) / maxMag);
    sSat.t = ad::satFor (e.style, e.drive); sBias.t = m.satBias;
    const float age = 1.0f + 0.6f * e.drive;                       // DRIVE also ages the tape a little
    sWow.t = (float) (m.wowMs * age * 0.001 * sr); sFlut.t = (float) (m.flutMs * age * 0.001 * sr);
    sPP.t = m.pingPong;
    sDryG.t = juce::jmin (1.0f, 2.0f * (1.0f - e.mix)); sWetG.t = juce::jmin (1.0f, 2.0f * e.mix);
    sWidth.t = e.width; sFreeze.t = e.freeze ? 1.0f : 0.0f;
    sSend.t = e.throws ? 0.0f : 1.0f;
    sLpLog.t = std::log (lp); sHpLog.t = std::log (hp); sLpQ.t = m.lpQ; sHpQ.t = m.hpQ;
    wowHz = m.wowHz; flutHz = m.flutHz; stPhase = m.stereoPhase;
    duckDepth = 20.0f * e.duck;

    double secs = ad::noteBeats[juce::jlimit (0, ad::numNotes - 1, e.note)] * 60.0 / bpm;
    while (secs > maxDelaySecs) secs *= 0.5;
    const double noteSamples = secs * sr;
    const double gd = ad::groupDelay (lp, m.lpQ, hp, m.hpQ, 1000.0, sr);
    dTarget = juce::jmax (4.0, noteSamples - gd);

    delayMs.store ((float) (secs * 1000.0));
    repeatGain.store ((float) (sFb.t * ad::loopMag (lp, m.lpQ, hp, m.hpQ, 1000.0)));

    if (snap)
    {
        for (auto* s : { &sFb, &sSat, &sBias, &sWow, &sFlut, &sPP, &sDryG, &sWetG, &sWidth, &sFreeze, &sSend, &sLpLog, &sHpLog, &sLpQ, &sHpQ }) s->snap();
        dA = dB = dTarget; fading = false; xf = 1.0f;
        for (int c = 0; c < 2; ++c) { lpf[c].set (lp, m.lpQ, sr); hpf[c].set (hp, m.hpQ, sr); }
    }
}

// ── Audio ─────────────────────────────────────────────────────────────────────
void AutoDelayProAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = juce::jmin (2, getTotalNumInputChannels(), buffer.getNumChannels());
    const int numCh = juce::jmin (2, getTotalNumOutputChannels(), buffer.getNumChannels());
    for (int c = numCh; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (numCh <= 0 || n <= 0 || line[0].empty()) return;

    // Tempo: the host's, or a steady 120 BPM in the standalone app.
    double bpm = 120.0;
    if (wrapperType != wrapperType_Standalone)
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm()) bpm = *b;
    if (forcedBpm > 0.0) bpm = forcedBpm;
    bpm = juce::jlimit (20.0, 400.0, bpm);
    hostBpm.store (bpm);

    // Locked features fall back to their Lite values, so automation or an old session can't bypass the lock.
    const bool pro = proUnlocked.load();
    const Effective e = resolve (pro, bpm);
    updateTargets (e, bpm, ! prepared);
    prepared = true;

    const auto& m = ad::model (e.style);
    if (e.style != lastStyle) lastStyle = e.style;
    const bool glide = m.glide;
    const double noteSamples = dTarget + ad::groupDelay (std::exp (sLpLog.t), sLpQ.t, std::exp (sHpLog.t), sHpQ.t, 1000.0, sr);

    float* data[2] = { buffer.getWritePointer (0), buffer.getWritePointer (numCh > 1 ? 1 : 0) };
    const float* inData[2] = { numIn > 0 ? buffer.getReadPointer (0) : nullptr, numIn > 1 ? buffer.getReadPointer (1) : nullptr };
    juce::int64 now = clock.load();
    const double wowInc = 2.0 * ad::pi * wowHz / sr, flutInc = 2.0 * ad::pi * flutHz / sr;
    const float c16 = 1.0f - std::pow (1.0f - cFast, 16.0f);
    float bIn = 0, bOut = 0, bWet = 0;

    for (int i = 0; i < n; ++i, ++now)
    {
        const float inL = inData[0] != nullptr ? inData[0][i] : 0.0f;
        const float inR = inData[1] != nullptr ? inData[1][i] : inL;
        const float xAbs = juce::jmax (std::abs (inL), std::abs (inR));

        // ── smoothed controls ──
        const float fb = sFb.next (cFast), sat = sSat.next (cFast), bias = sBias.next (cFast);
        const float wow = sWow.next (cFast), flut = sFlut.next (cFast), pp = sPP.next (cFast);
        const float dryG = sDryG.next (cFast), wetG = sWetG.next (cFast), wd = sWidth.next (cFast);
        const float fz = sFreeze.next (cFast), send = sSend.next (cFast) * (1.0f - fz);
        if (++coefCount >= 16)
        {
            coefCount = 0;
            const float lpHz = std::exp (sLpLog.next (c16)), hpHz = std::exp (sHpLog.next (c16));
            const float lq = sLpQ.next (c16), hq = sHpQ.next (c16);
            for (int c = 0; c < 2; ++c) { lpf[c].set (lpHz, lq, sr); hpf[c].set (hpHz, hq, sr); }
        }

        // ── delay time: tape styles glide (pitch bend), the others crossfade to the new time ──
        if (fading)
        {
            xf += xfStep;
            if (xf >= 1.0f) { xf = 1.0f; dA = dB; fading = false; }
        }
        if (! fading)
        {
            const double diff = dTarget - dA;
            if (glide)
                dA += juce::jlimit (-0.5, 0.5, diff * glideCoef);
            else if (std::abs (diff) > 0.005 * dA)
            { dB = dTarget; xf = 0.0f; fading = true; }
            else
                dA += diff * smallGlideCoef;
        }

        // ── read heads (+ wow / flutter) ──
        wowPh += wowInc; if (wowPh > 2.0 * ad::pi) wowPh -= 2.0 * ad::pi;
        flutPh += flutInc; if (flutPh > 2.0 * ad::pi) flutPh -= 2.0 * ad::pi;
        float raw[2];
        const float gA = fading ? std::cos (0.5f * (float) ad::pi * xf) : 1.0f, gB = fading ? std::sin (0.5f * (float) ad::pi * xf) : 0.0f;
        for (int c = 0; c < 2; ++c)
        {
            const double mod = wow * std::sin (wowPh + c * stPhase) + flut * std::sin (flutPh + c * stPhase * 0.7);
            float r = ad::readHermite (line[c].data(), size, w, dA + mod) * gA;
            if (fading) r += ad::readHermite (line[c].data(), size, w, dB + mod) * gB;
            raw[c] = r;
        }

        // ── inside the loop: saturate, LP, HP (each repeat a little darker and softer) ──
        float proc[2];
        for (int c = 0; c < 2; ++c)
        {
            const float s = ad::loopSat (raw[c], sat, bias);
            float lp, hp, dummy;
            lpf[c].tick (s, lp, dummy);
            hpf[c].tick (lp, dummy, hp);
            proc[c] = hp + (ad::loopSat (raw[c], 0.0f, 0.0f) - hp) * fz;   // FREEZE: the loop plays back untouched
        }
        const float g = fb + (1.0f - fb) * fz;

        // ── write: input (unless throws / freeze) + feedback; ping pong sends mono left and crosses over ──
        const float mono = 0.5f * (inL + inR);
        const float sL = send * ((1.0f - pp) * inL + pp * mono), sR = send * (1.0f - pp) * inR;
        const float fL = (1.0f - pp) * proc[0] + pp * proc[1], fR = (1.0f - pp) * proc[1] + pp * proc[0];
        line[0][(size_t) w] = sL + g * fL + inject[0][(size_t) w]; inject[0][(size_t) w] = 0.0f;
        line[1][(size_t) w] = sR + g * fR + inject[1][(size_t) w]; inject[1][(size_t) w] = 0.0f;

        // ── detectors: level (ducking), phrases (throws), onsets (display) ──
        const int hIdx = (int) (now % histSize);
        hist[0][(size_t) hIdx] = inL; hist[1][(size_t) hIdx] = inR;
        duckEnv += (xAbs - duckEnv) * (xAbs > duckEnv ? duckEnvA : duckEnvR);
        peakTrack = duckEnv > peakTrack ? duckEnv : juce::jmax (1.0e-3f, peakTrack * peakRel);
        {
            // Phrase ends are found on a fast RMS level against the recent level (a peak follower that falls
            // ~17 dB/s). A word is sounding while the level stays within ~4 dB of it. A phrase ends when the
            // level dips under that for 85 ms and at some point sits 7 dB+ down: the voice stopped, even if
            // printed reverb carries on underneath. Held notes and pads never fall that fast, so they never throw.
            phMs += (0.5f * (inL * inL + inR * inR) - phMs) * phMsC;
            const float lev = std::sqrt (phMs);
            phRef = lev > phRef ? lev : phRef * phRefRel;
            const float flr = juce::jmax (3.0e-4f, peakTrack * 0.005f);
            const float thr = juce::jmax (flr, phRef * wordRatio);
            bool active = wordOn ? lev > thr * 0.85f : lev > thr;
            // A new phrase needs an onset (a reverb tail that the falling reference catches up with isn't one).
            if (active && ! inPhrase && lev < onsetRatio * phSlow) active = false;
            phSlow += (lev - phSlow) * phSlowC;
            if (active && ! wordOn) { wordOn = true; wordStart = now; if (! inPhrase) { inPhrase = true; phraseStart = now; } }
            if (! active && wordOn) { wordOn = false; dipDeep = false; }
            if (active) lastActive = now;
            else if (lev < juce::jmax (flr, phRef * deepRatio)) dipDeep = true;
            if (inPhrase && ! active && dipDeep && (double) (now - lastActive) >= gapSecs * sr)
            {
                inPhrase = false;
                if ((double) (lastActive - phraseStart) >= minPhrase * sr && e.throws && fz < 0.5f)
                    fireThrow ((int) (noteSamples + 0.5), hIdx, now);
            }
        }
        onFast += (xAbs - onFast) * (xAbs > onFast ? 0.05f : 0.002f);
        onSlow += (xAbs - onSlow) * 0.0004f;
        if (! e.throws && onFast > 1.0e-3f && onFast > 2.0f * onSlow + 1.0e-4f && (double) (now - lastOnset) > 0.12 * sr)
        {
            lastOnset = now; lastHit.store (now); lastHitFirstRepeat.store (1); lastHitIsThrow.store (false);
        }

        // ── ducking: the repeats drop while you play and bloom in the gaps ──
        {
            const float envDb = juce::Decibels::gainToDecibels (duckEnv, -120.0f);
            const float pkDb = juce::Decibels::gainToDecibels (peakTrack, -120.0f);
            const float a = envDb < -60.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, (envDb - (pkDb - 30.0f)) / 18.0f);
            const float target = -duckDepth * a;
            duckGainDb += (target - duckGainDb) * (target < duckGainDb ? duckAtk : duckRel);
        }
        const float duckG = juce::Decibels::decibelsToGain (duckGainDb);

        // ── wet: width (M/S), ducking ──
        float wl = proc[0], wr = proc[1];
        {
            const float M = 0.5f * (wl + wr);
            float S = 0.5f * (wl - wr) * juce::jmin (1.0f, wd);
            const float d0 = decor[0].tick (M), d1 = decor[1].tick (d0);
            if (wd > 1.0f) S += (wd - 1.0f) * 0.9f * (d1 - M) * 0.5f;
            wl = (M + S) * duckG; wr = (M - S) * duckG;
        }
        if (wetTrace != nullptr) wetTrace->push_back (0.5f * (wl + wr));

        const float dry[2] = { inL, inR };
        const float wet[2] = { wl, wr };
        for (int c = 0; c < numCh; ++c)
        {
            const int src = numCh == 1 ? 0 : c;
            float out = dry[src] * dryG + (numCh == 1 ? 0.5f * (wl + wr) : wet[c]) * wetG;
            // Safety ceiling: soft above -1 dBFS, never past 0 dBFS.
            const float a = std::abs (out), t = 0.891f;
            if (a > t) out = std::copysign (t + (1.0f - t) * std::tanh ((a - t) / (1.0f - t)), out);
            data[c][i] = out;
            bOut = juce::jmax (bOut, std::abs (out));
        }
        bIn = juce::jmax (bIn, xAbs);
        bWet = juce::jmax (bWet, juce::jmax (std::abs (wl), std::abs (wr)));

        if (++w >= size) w = 0;
    }
    clock.store (now);
    inPeak.store (bIn); outPeak.store (bOut); wetPeak.store (bWet); duckDb.store (duckGainDb);
}

// AUTO THROWS: a phrase just ended. Put its last word into the delay so its repeats land exactly where
// they would have (on the grid, k whole repeats after the word), without the rest of the phrase.
// The word is already in the past: if its first repeat hasn't been read yet it goes straight into the
// line; otherwise it is placed one or more repeats later (the `inject` ring sits ahead of the write head).
void AutoDelayProAudioProcessor::fireThrow (int N, int histNow, juce::int64 nowAbs)
{
    const int pre = (int) (preRollSecs * sr), maxTail = (int) (maxTailSecs * sr);
    // ages = samples before the current one
    const juce::int64 ageStartWord = nowAbs - (wordStart - pre), ageLastActive = nowAbs - lastActive;
    juce::int64 age0 = juce::jmin (ageStartWord, ageLastActive + maxTail);
    age0 = juce::jmin (age0, (juce::int64) histSize - 2);
    if (age0 <= 0 || N <= 4) return;
    const int len = (int) age0 + 1;               // from the word start up to and including now

    // first repeat k whose slot hasn't been read yet
    int k = 1;
    while ((double) (-age0) + (double) (k - 1) * N + dA < 3.0) ++k;
    const juce::int64 shift = (juce::int64) (k - 1) * N;

    const int fadeIn = juce::jmax (1, (int) (0.010 * sr)), fadeOut = juce::jmax (1, (int) (0.015 * sr));
    const float pp = sPP.v;
    for (int j = 0; j < len; ++j)
    {
        const juce::int64 age = age0 - j;                         // this tail sample's age
        int h = histNow - (int) age; while (h < 0) h += histSize;
        float win = 1.0f;
        if (j < fadeIn) win = 0.5f - 0.5f * std::cos ((float) ad::pi * (float) j / (float) fadeIn);
        if (len - 1 - j < fadeOut) win *= 0.5f - 0.5f * std::cos ((float) ad::pi * (float) (len - 1 - j) / (float) fadeOut);
        const float xl = hist[0][(size_t) h] * win, xr = hist[1][(size_t) h] * win, mono = 0.5f * (xl + xr);
        const float vl = (1.0f - pp) * xl + pp * mono, vr = (1.0f - pp) * xr;
        const juce::int64 rel = -age + shift;                    // relative to the current write index
        int idx = (int) (((juce::int64) w + rel) % size); if (idx < 0) idx += size;
        auto* dst = rel <= 0 ? line : inject;   // arrays decay to pointers (MSVC needs auto*, not auto&)
        dst[0][(size_t) idx] += vl; dst[1][(size_t) idx] += vr;
    }
    const juce::int64 srcStart = nowAbs - age0;
    lastHit.store (srcStart + juce::jmin ((juce::int64) pre, age0)); lastHitFirstRepeat.store (k); lastHitIsThrow.store (true);
    throwCount.fetch_add (1);
    if (throwTrace != nullptr) throwTrace->push_back (srcStart);
}

// ── State ─────────────────────────────────────────────────────────────────────
void AutoDelayProAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, destData);
}

void AutoDelayProAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AutoDelayProAudioProcessor::createEditor() { return new AutoDelayProAudioProcessorEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AutoDelayProAudioProcessor(); }
