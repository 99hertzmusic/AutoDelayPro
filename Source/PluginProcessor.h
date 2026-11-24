#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "Features.h"
#include "DelayDSP.h"
#include "Licence/Licence.h"

// AutoDelay Pro: the delay that does the maths for you.
//  Lite (free): CLEAN + PING PONG, AUTO time from the host tempo, FEEDBACK, MIX, AUTO DUCKING (fixed).
//  Pro (key or 7-day trial): SLAPBACK / TAPE / DUB / ANALOG / LO-FI, AUTO THROWS, ducking amount,
//  note-value override, TONE, DRIVE, WIDTH and FREEZE. The split itself lives in Features.h (ad::gate).
class AutoDelayProAudioProcessor : public juce::AudioProcessor
{
public:
    AutoDelayProAudioProcessor();
    ~AutoDelayProAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "AutoDelay Pro"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ── Licence ─────────────────────────────────────────────────────────────
    hz::Licence licence { "autodelaypro" };
    hz::LicenceInfo licenceInfo;                 // message thread
    std::atomic<bool> proUnlocked { false };     // audio thread reads this
    void setLicenceInfo (const hz::LicenceInfo& i) { licenceInfo = i; proUnlocked.store (i.unlocked()); }

    // ── What actually runs, after the Lite/Pro gate and AUTO are resolved ───
    struct Effective
    {
        int   style = ad::clean;
        bool  timeAuto = true; int note = ad::n8D;
        float feedback = 0.4f, mix = 0.3f, duck = 0.6f, tone = 0.0f, drive = 0.0f, width = 1.0f;
        bool  throws = false, freeze = false;
    };
    Effective resolve (bool pro, double bpm) const;

    // ── For the editor ─────────────────────────────────────────────────────
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<float> delayMs { 375.0f }, repeatGain { 0.4f };   // repeat spacing + level ratio per repeat (1 kHz)
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, wetPeak { 0.0f }, duckDb { 0.0f };
    std::atomic<juce::int64> clock { 0 };        // samples processed
    std::atomic<juce::int64> lastHit { -1 };     // when the last note / throw went into the delay (clock samples)
    std::atomic<int> lastHitFirstRepeat { 1 };   // throws: the first repeat you hear (1 or later)
    std::atomic<bool> lastHitIsThrow { false };
    std::atomic<int> throwCount { 0 };
    double getSampleRateSafe() const { return sr; }

    // ── Test hooks ─────────────────────────────────────────────────────────
    std::vector<float>* wetTrace = nullptr;      // when set, per-sample wet (L+R)/2 after ducking is appended (not real-time safe)
    std::vector<juce::int64>* throwTrace = nullptr;   // clock sample of each throw's source start
    double forcedBpm = 0.0;                      // > 0: use this instead of the host (tests)

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void updateTargets (const Effective& e, double bpm, bool snap);
    void fireThrow (int N, int histNow, juce::int64 nowAbs);

    std::atomic<float>* pStyle = nullptr; std::atomic<float>* pTime = nullptr; std::atomic<float>* pFeedback = nullptr;
    std::atomic<float>* pMix = nullptr; std::atomic<float>* pDuck = nullptr; std::atomic<float>* pThrows = nullptr;
    std::atomic<float>* pTone = nullptr; std::atomic<float>* pDrive = nullptr; std::atomic<float>* pWidth = nullptr;
    std::atomic<float>* pFreeze = nullptr;

    double sr = 44100.0;
    bool prepared = false;

    // Delay lines (+ a ring for throws placed ahead of the write head)
    std::vector<float> line[2], inject[2];
    int size = 1, w = 0;

    // Read heads: A plays, B is faded in on a jump (non-tape styles); tape styles glide A.
    double dA = 1000.0, dB = 1000.0, dTarget = 1000.0;
    float xf = 1.0f, xfStep = 0.001f; bool fading = false;
    double glideCoef = 0.0, smallGlideCoef = 0.0;
    int lastStyle = -1;
    float lastLpT = -1.0f, lastHpT = -1.0f; double maxMag = 1.0;

    // Smoothed loop / output controls (one-pole, per sample)
    struct Sm { float v = 0, t = 0; inline float next (float c) { v += (t - v) * c; return v; } void snap() { v = t; } };
    Sm sFb, sSat, sBias, sWow, sFlut, sPP, sDryG, sWetG, sWidth, sFreeze, sSend, sLpLog, sHpLog, sLpQ, sHpQ;
    float cFast = 0.01f;            // ~20 ms
    float wowHz = 0, flutHz = 0, stPhase = 0;
    double wowPh = 0, flutPh = 0;
    ad::SVF lpf[2], hpf[2];
    int coefCount = 0;
    ad::AP1 decor[2];

    // Ducking
    float duckEnv = 0, duckGainDb = 0, duckAtk = 0, duckRel = 0, duckEnvA = 0, duckEnvR = 0;
    float peakTrack = 1.0e-4f, peakRel = 0;
    float duckDepth = 12.0f;

    // Phrase / throw detector
    std::vector<float> hist[2]; int histSize = 1;
    float phMs = 0, phMsC = 0;          // fast (10 ms) mean square
    float phRef = 0, phRefRel = 0;      // recent level: peak follower falling ~17 dB/s
    float phSlow = 0, phSlowC = 0;      // 40 ms smoothed level (onsets)
    bool  inPhrase = false, wordOn = false, dipDeep = false;
    juce::int64 phraseStart = 0, lastActive = 0, wordStart = 0;
    // UI onset detector
    float onFast = 0, onSlow = 0; juce::int64 lastOnset = -100000;


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoDelayProAudioProcessor)
};
