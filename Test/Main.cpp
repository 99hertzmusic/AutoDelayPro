// AutoDelay Pro offline test app.
// Usage: AutoDelayProTest <outdir> [quick] [name=key ...]
//        AutoDelayProTest demos <outdir> <vocal.wav> <vocalBpm> <synth.wav> <synthBpm>
//  - repeats land exactly on the host grid (several tempos x note values, sub-sample)
//  - every repeat darker than the last (HF energy per repeat), per style
//  - FEEDBACK 100 % stays bounded, tails ring out and die away
//  - ducking depth while playing vs in the gaps
//  - AUTO THROWS only catch the last word of a phrase
//  - no clicks on style / time / control changes, save / restore, Lite gating, licence keys, UI snapshots
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cmath>
#include <map>

using APP = AutoDelayProAudioProcessor;
static constexpr double SR = 48000.0;
static constexpr int BS = 256;
static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

using Buf = juce::AudioBuffer<float>;
static const double twoPi = juce::MathConstants<double>::twoPi;

// ── Helpers ──────────────────────────────────────────────────────────────────
static void setParam (APP& p, const char* id, float plain)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

static void setPro (APP& p, int kind)
{
    hz::LicenceInfo li; li.kind = (hz::LicenceInfo::Kind) kind; li.email = "buyer@example.com";
    li.expires = kind == hz::LicenceInfo::trial ? juce::Time::currentTimeMillis() / 1000 + 5 * 86400 - 60 : 0;
    p.setLicenceInfo (li);
}

static std::unique_ptr<APP> makeProc (bool pro, double bpm = 120.0, double rate = SR, std::function<void (APP&)> setup = nullptr)
{
    auto p = std::make_unique<APP>();
    setPro (*p, pro ? hz::LicenceInfo::pro : hz::LicenceInfo::none);
    p->forcedBpm = bpm;
    if (setup) setup (*p);
    p->setRateAndBufferSizeDetails (rate, BS);
    p->prepareToPlay (rate, BS);
    return p;
}

static Buf run (APP& p, const Buf& in, int len = -1, int bs = BS)
{
    if (len < 0) len = in.getNumSamples();
    Buf out (2, len), blk (2, bs);
    juce::MidiBuffer m;
    for (int pos = 0; pos < len; pos += bs)
    {
        const int n = juce::jmin (bs, len - pos);
        blk.setSize (2, n, false, false, true);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) blk.setSample (c, i, pos + i < in.getNumSamples() ? in.getSample (c, pos + i) : 0.0f);
        p.processBlock (blk, m);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }
    return out;
}

static void writeWav (const juce::File& f, const Buf& b, double rate = SR)
{
    f.deleteFile();
    juce::WavAudioFormat w;
    if (auto os = std::unique_ptr<juce::OutputStream> (f.createOutputStream().release()))
        if (auto wr = std::unique_ptr<juce::AudioFormatWriter> (w.createWriterFor (os.get(), rate, 2, 24, {}, 0)))
        { os.release(); wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
}

static double rmsDb (const std::vector<float>& x, int a, int b)
{
    a = juce::jmax (0, a); b = juce::jmin ((int) x.size(), b);
    double s = 0; for (int i = a; i < b; ++i) s += (double) x[(size_t) i] * x[(size_t) i];
    return 10 * std::log10 (s / juce::jmax (1, b - a) + 1e-30);
}

static double noteSamples (int note, double bpm, double rate = SR) { return ad::noteBeats[note] * 60.0 / bpm * rate; }

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad hp (double f, double q, double fs)
    {
        Biquad b; const double w = twoPi * f / fs, al = std::sin (w) / (2 * q), c = std::cos (w), a0 = 1 + al;
        b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; return b;
    }
    double p (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// ── 1. Timing: every repeat exactly on the grid ─────────────────────────────
// 1 kHz Gaussian burst (sigma 1 ms) -> wet only. Arrival time of repeat k = peak of the burst-envelope
// cross-correlation (quadrature, parabolic refinement). Must be k * note length, within 0.5 sample.
static void timingTests()
{
    std::printf ("\n== Repeats on the host grid (error per repeat, samples @ 48 kHz) ==\n");
    const int t0 = 4800;
    const double sig = 0.001 * SR;
    const int half = (int) (5 * sig);
    std::vector<double> bc, bs;
    for (int i = -half; i <= half; ++i)
    {
        const double env = std::exp (-0.5 * (i / sig) * (i / sig));
        bc.push_back (env * std::cos (twoPi * 1000.0 * i / SR)); bs.push_back (env * std::sin (twoPi * 1000.0 * i / SR));
    }
    double worst = 0; int count = 0; bool allOk = true;
    for (int style : { (int) ad::clean, (int) ad::pingPong, (int) ad::slapback })
        for (double bpm : { 90.0, 120.0, 128.0, 140.0, 174.0 })
            for (int note : { (int) ad::n16, (int) ad::n8T, (int) ad::n8D, (int) ad::n4, (int) ad::n2 })
            {
                auto p = makeProc (true, bpm, SR, [&] (APP& q)
                { setParam (q, "style", (float) style); setParam (q, "time", (float) note); setParam (q, "feedback", 70);
                  setParam (q, "mix", 100); setParam (q, "duck", 0); });
                const double N = noteSamples (note, bpm);
                const int len = t0 + (int) (4.3 * N) + half + 10;
                Buf in (2, len); in.clear();
                for (int i = -half; i <= half; ++i) { const float v = (float) (0.5 * bc[(size_t) (i + half)]); in.setSample (0, t0 + i, v); in.setSample (1, t0 + i, v); }
                std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
                run (*p, in, len);
                juce::String errs;
                for (int k = 1; k <= 4; ++k)
                {
                    const double expect = t0 + k * N;
                    auto corr = [&] (int lag)
                    {
                        double c = 0, s = 0;
                        for (int i = -half; i <= half; ++i)
                        {
                            const int idx = lag + i; if (idx < 0 || idx >= (int) wet.size()) continue;
                            c += wet[(size_t) idx] * bc[(size_t) (i + half)]; s += wet[(size_t) idx] * bs[(size_t) (i + half)];
                        }
                        return std::sqrt (c * c + s * s);
                    };
                    int best = (int) std::round (expect); double bv = -1;
                    for (int lag = (int) expect - 60; lag <= (int) expect + 60; ++lag) { const double v = corr (lag); if (v > bv) { bv = v; best = lag; } }
                    const double ym = corr (best - 1), y0 = bv, yp = corr (best + 1);
                    const double frac = 0.5 * (ym - yp) / (ym - 2 * y0 + yp);
                    const double err = best + frac - expect;
                    worst = juce::jmax (worst, std::abs (err)); ++count;
                    if (std::abs (err) >= 0.5) allOk = false;
                    errs << juce::String (err, 2) << " ";
                }
                if (bpm == 120.0 || bpm == 174.0)
                    std::printf ("  %-9s %5.0f BPM %-5s (%7.1f ms): %s\n", ad::styleName (style), bpm, ad::noteName (note), N / SR * 1000.0, errs.toRawUTF8());
            }
    check (allOk, "all " + juce::String (count) + " repeats (3 styles x 5 tempos x 5 note values x 4 repeats) within 0.5 sample of the grid (worst " + juce::String (worst, 3) + ")");

    // Host tempo is read from the playhead (no forced tempo): 128 BPM, CLEAN AUTO = 1/8D = 351.6 ms.
    struct PH : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (128.0); i.setPpqPosition (0.0); i.setIsPlaying (true); return i; }
    } ph;
    auto p = makeProc (false, 0.0);
    p->setPlayHead (&ph);
    Buf in (2, BS); in.clear(); run (*p, in);
    check (std::abs (p->delayMs.load() - 60000.0f / 128.0f * 0.75f) < 0.01f, "reads the host tempo: 128 BPM AUTO 1/8D = " + juce::String (p->delayMs.load(), 2) + " ms");
    // Lite AUTO per style at 120 BPM
    std::printf ("  AUTO note value per style @ 120 / 90 BPM: ");
    for (int s = 0; s < ad::numStyles; ++s) std::printf ("%s %s/%s  ", ad::styleName (s), ad::noteName (ad::autoNote (s, 120)), ad::noteName (ad::autoNote (s, 90)));
    std::printf ("\n");
}

// ── 2. Each repeat darker + the sound of each style ──────────────────────────
struct StyleReport { double firstDb, dropDb, hfDropDb, widthCorr, thdPct, wowCents; };
static std::map<int, StyleReport> report;

static void darkTests()
{
    std::printf ("\n== Each repeat darker than the last (white-noise burst, FEEDBACK 75, 1/4 @ 120) ==\n");
    const int t0 = 4800, burst = (int) (0.03 * SR);
    juce::Random rnd (3);
    Buf in (2, t0 + burst); in.clear();
    for (int i = 0; i < burst; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (twoPi * i / burst));
        const float v = 0.5f * w * (rnd.nextFloat() * 2 - 1);
        in.setSample (0, t0 + i, v); in.setSample (1, t0 + i, v);
    }
    double inE = 0; for (int i = 0; i < burst; ++i) inE += 2.0 * in.getSample (0, t0 + i) * in.getSample (0, t0 + i);
    for (int s = 0; s < ad::numStyles; ++s)
    {
        auto p = makeProc (true, 120.0, SR, [&] (APP& q)
        { setParam (q, "style", (float) s); setParam (q, "time", (float) ad::n4); setParam (q, "feedback", 75); setParam (q, "mix", 100); setParam (q, "duck", 0); });
        const int N = (int) noteSamples (ad::n4, 120);
        const int len = t0 + 6 * N + N / 2;
        std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
        auto out = run (*p, in, len);
        double lvl[6], hf[6];
        juce::String line;
        bool darker = true, softer = true;
        for (int k = 1; k <= 5; ++k)
        {
            auto hp1 = Biquad::hp (4000, 0.54, SR), hp2 = Biquad::hp (4000, 1.31, SR);
            double e = 0, h = 0;
            auto hq1 = Biquad::hp (4000, 0.54, SR), hq2 = Biquad::hp (4000, 1.31, SR);
            for (int i = t0 + k * N - N / 4; i < t0 + k * N + N / 2; ++i)
            {
                const double x = out.getSample (0, i), xr = out.getSample (1, i);   // L and R powers (no L+R comb effects)
                const double y = hp2.p (hp1.p (x)), yr = hq2.p (hq1.p (xr));
                if (i >= t0 + k * N - 400) { e += x * x + xr * xr; h += y * y + yr * yr; }
            }
            lvl[k] = 10 * std::log10 (e / inE + 1e-30);
            hf[k] = 10 * std::log10 (h / (e + 1e-30) + 1e-30);
            line << "R" << k << " " << juce::String (lvl[k], 1) << " dB / HF " << juce::String (hf[k], 1) << "  ";
            if (k > 1 && hf[k] >= hf[k - 1] - 0.05) darker = false;
            if (k > 1 && lvl[k] >= lvl[k - 1]) softer = false;
        }
        std::printf ("  %-9s %s\n", ad::styleName (s), line.toRawUTF8());
        check (darker, juce::String (ad::styleName (s)) + ": every repeat darker than the one before (HF share falls each time)");
        check (softer, juce::String (ad::styleName (s)) + ": every repeat quieter than the one before");
        // stereo: correlation of wet L/R on repeats 1..4 (ping pong alternates: low / negative)
        double lr = 0, ll = 0, rr = 0;
        for (int i = t0 + N - 400; i < t0 + 5 * N; ++i) { const double l = out.getSample (0, i), r = out.getSample (1, i); lr += l * r; ll += l * l; rr += r * r; }
        report[s].firstDb = lvl[1]; report[s].dropDb = (lvl[5] - lvl[1]) / 4.0; report[s].hfDropDb = (hf[5] - hf[1]) / 4.0;
        report[s].widthCorr = lr / std::sqrt (ll * rr + 1e-30);
    }

    // Saturation of the first repeat (-6 dBFS 220 Hz sine, FEEDBACK 0) and pitch wobble (1 kHz, zero-crossing rate)
    for (int s = 0; s < ad::numStyles; ++s)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            const double f0 = pass == 0 ? 220.0 : 1000.0;
            auto p = makeProc (true, 120.0, SR, [&] (APP& q)
            { setParam (q, "style", (float) s); setParam (q, "time", (float) ad::n4); setParam (q, "feedback", 0); setParam (q, "mix", 100); setParam (q, "duck", 0); });
            const int len = (int) (4.0 * SR);
            Buf in (2, len);
            for (int i = 0; i < len; ++i) { const float v = 0.5f * (float) std::sin (twoPi * f0 * i / SR); in.setSample (0, i, v); in.setSample (1, i, v); }
            auto o = run (*p, in, len);
            std::vector<float> wet ((size_t) len);
            for (int i = 0; i < len; ++i) wet[(size_t) i] = o.getSample (0, i);   // left channel only
            const int a0 = (int) (1.5 * SR), a1 = len;
            if (pass == 0)
            {
                // harmonics 2..10 (+-15 Hz) vs the fundamental (+-30 Hz, wow spreads it a little), one long FFT
                constexpr int order = 17, NF = 1 << order;
                juce::dsp::FFT fft (order);
                std::vector<float> fb ((size_t) NF * 2, 0.0f);
                for (int i = 0; i < NF && a0 + i < a1; ++i) fb[(size_t) i] = wet[(size_t) (a0 + i)] * (float) (0.5 - 0.5 * std::cos (twoPi * i / NF));
                fft.performFrequencyOnlyForwardTransform (fb.data());
                const double hz = SR / NF;
                auto band = [&] (double f, double w) { double en = 0; for (int b = (int) ((f - w) / hz); b <= (int) ((f + w) / hz); ++b) en += (double) fb[(size_t) b] * fb[(size_t) b]; return en; };
                double hsum = 0; for (int h = 2; h <= 10; ++h) hsum += band (f0 * h, 15.0);
                report[s].thdPct = 100.0 * std::sqrt (hsum / band (f0, 30.0));
            }
            else
            {
                // instantaneous frequency from zero crossings over 2.5 s: max deviation in cents
                // band-pass around 1 kHz first so saturation harmonics can't add zero crossings
                std::vector<double> y (wet.size(), 0.0);
                { const double w0 = twoPi * f0 / SR, al = std::sin (w0) / (2 * 3.0), c = std::cos (w0), a0n = 1 + al;
                  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                  for (size_t i = 0; i < wet.size(); ++i) { const double x = wet[i], v = (al * x - al * x2 - (-2 * c) * y1 - (1 - al) * y2) / a0n; x2 = x1; x1 = x; y2 = y1; y1 = v; y[i] = v; } }
                std::vector<double> zc;
                for (int i = a0 + 1; i < a1; ++i) if (y[(size_t) i - 1] < 0 && y[(size_t) i] >= 0) zc.push_back (i - 1 + y[(size_t) i - 1] / (y[(size_t) i - 1] - y[(size_t) i]));
                double dev = 0;
                for (size_t i = 10; i < zc.size(); ++i) { const double f = 10.0 * SR / (zc[i] - zc[i - 10]); dev = std::max (dev, std::abs (1200.0 * std::log2 (f / f0))); }
                report[s].wowCents = dev;
            }
        }
    }
    std::printf ("\n  Style sound sheet (measured):\n  %-9s %9s %12s %13s %9s %10s %10s\n", "style", "1st rep", "drop/repeat", "HF/repeat", "L/R corr", "THD 1st", "wobble");
    for (int s = 0; s < ad::numStyles; ++s)
        std::printf ("  %-9s %6.1f dB %9.1f dB %10.1f dB %9.2f %8.2f %% %7.1f ct\n", ad::styleName (s), report[s].firstDb, report[s].dropDb, report[s].hfDropDb,
                     report[s].widthCorr, report[s].thdPct, report[s].wowCents);
    check (report[ad::clean].wowCents < 0.5 && report[ad::tape].wowCents > 1.0, "CLEAN holds pitch, TAPE wobbles (" + juce::String (report[ad::tape].wowCents, 1) + " cents)");
    check (report[ad::pingPong].widthCorr < 0.2, "PING PONG repeats bounce L/R (L/R correlation " + juce::String (report[ad::pingPong].widthCorr, 2) + ")");
}

// ── 3. FEEDBACK 100 % never runs away, tails die away ────────────────────────
static void boundTests()
{
    std::printf ("\n== FEEDBACK 100 %%: bounded, no runaway, tails still die away ==\n");
    const int on = (int) (10 * SR), len = (int) (22 * SR);
    juce::Random rnd (9);
    Buf in (2, len); in.clear();
    for (int i = 0; i < on; ++i) { in.setSample (0, i, 0.98f * (rnd.nextFloat() * 2 - 1)); in.setSample (1, i, 0.98f * (rnd.nextFloat() * 2 - 1)); }
    bool allOk = true;
    for (int s = 0; s < ad::numStyles; ++s)
        for (int drive : { 0, 100 })
        {
            auto p = makeProc (true, 120.0, SR, [&] (APP& q)
            { setParam (q, "style", (float) s); setParam (q, "feedback", 100); setParam (q, "mix", 100); setParam (q, "duck", 0); setParam (q, "drive", (float) drive); setParam (q, "tone", 100); });
            std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
            auto out = run (*p, in, len);
            float peak = 0; bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < len; ++i) { const float v = out.getSample (c, i); if (! std::isfinite (v)) finite = false; peak = juce::jmax (peak, std::abs (v)); }
            double win[22]; for (int k = 0; k < 22; ++k) win[k] = rmsDb (wet, (int) (k * SR), (int) ((k + 1) * SR));
            const double grow = juce::jmax (win[7], win[8], win[9]) - juce::jmin (win[7], win[8], win[9]);
            const double decay = win[11] - win[21];
            const bool ok = finite && peak <= 1.0f && grow < 2.0 && win[9] < 0.0 && decay > 3.0;
            if (! ok) allOk = false;
            std::printf ("  %-9s drive %3d: out peak %5.2f dBFS, wet rms last 3 s %5.1f/%5.1f/%5.1f dB, tail 1 s->11 s after stop falls %5.1f dB %s\n",
                         ad::styleName (s), drive, juce::Decibels::gainToDecibels (peak), win[7], win[8], win[9], decay, ok ? "" : "<-- FAIL");
        }
    check (allOk, "every style at FEEDBACK 100 (TONE bright, DRIVE 0/100): never past 0 dBFS, level steady (< 2 dB drift), tail keeps falling");

    // FREEZE holds (doesn't grow, doesn't fade)
    {
        auto p = makeProc (true, 120.0, SR, [&] (APP& q) { setParam (q, "feedback", 50); setParam (q, "mix", 100); setParam (q, "duck", 0); });
        Buf x (2, (int) (2 * SR)); x.clear();
        for (int i = 0; i < (int) SR; ++i) { const float v = 0.4f * (float) std::sin (twoPi * 330 * i / SR) * (rnd.nextFloat() * 0.5f + 0.5f); x.setSample (0, i, v); x.setSample (1, i, v); }
        std::vector<float> wet; wet.reserve ((size_t) (14 * SR)); p->wetTrace = &wet;
        run (*p, x, (int) (0.9 * SR));
        setParam (*p, "freeze", 1);
        Buf rest (2, (int) (12 * SR)); rest.clear();
        for (int i = 0; i < (int) (0.1 * SR); ++i) for (int c = 0; c < 2; ++c) rest.setSample (c, i, x.getSample (c, (int) (0.9 * SR) + i));
        run (*p, rest);
        const double a = rmsDb (wet, (int) (2 * SR), (int) (3 * SR)), b = rmsDb (wet, (int) (11 * SR), (int) (12 * SR));
        std::printf ("  FREEZE: wet %.1f dB at 1-2 s, %.1f dB 10 s later\n", a, b);
        check (std::abs (a - b) < 1.0 && a > -40, "FREEZE holds the repeats (" + juce::String (b - a, 2) + " dB over 10 s)");
    }
}

// ── 4. Tails ring out ────────────────────────────────────────────────────────
static void tailTests()
{
    std::printf ("\n== Tails ring out when the input stops (Lite defaults, 0.5 s phrase then silence) ==\n");
    auto p = makeProc (false);
    const int len = (int) (20 * SR), stop = (int) (0.5 * SR);
    Buf in (2, len); in.clear();
    juce::Random rnd (5);
    for (int i = 0; i < stop; ++i) { const float v = 0.4f * (float) std::sin (twoPi * 440 * i / SR) * (float) std::exp (-(i % 6000) / 2000.0); in.setSample (0, i, v); in.setSample (1, i, v); }
    std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
    auto out = run (*p, in, len);
    const int N = (int) noteSamples (ad::n8D, 120);
    juce::String line; bool falling = true; double prev = 1e9;
    for (int k = 1; k <= 6; ++k)
    {
        const double l = rmsDb (wet, stop + (k - 1) * N, stop + k * N);
        line << juce::String (l, 1) << " ";
        if (l >= prev) falling = false;
        prev = l;
    }
    float endPeak = 0; for (int c = 0; c < 2; ++c) for (int i = len - (int) SR; i < len; ++i) endPeak = juce::jmax (endPeak, std::abs (out.getSample (c, i)));
    std::printf ("  wet per repeat after the stop (dB): %s\n", line.toRawUTF8());
    check (rmsDb (wet, stop, stop + 2 * N) > -40.0, "repeats keep playing after the input stops (" + juce::String (rmsDb (wet, stop, stop + 2 * N), 1) + " dB)");
    check (falling, "each repeat after the stop is quieter than the last");
    check (endPeak < 1.0e-5f, "tail dies away to silence (peak in the last second " + juce::String (juce::Decibels::gainToDecibels (endPeak, -200.0f), 1) + " dBFS)");
    check (p->getTailLengthSeconds() >= 6.0, "reports a tail to the host (" + juce::String (p->getTailLengthSeconds()) + " s)");
}

// ── 5. Ducking ───────────────────────────────────────────────────────────────
static void duckTests()
{
    std::printf ("\n== Auto ducking: repeats drop while you play, bloom in the gaps (220 Hz, 2 s on / 2 s off) ==\n");
    const int len = (int) (8 * SR);
    Buf in (2, len); in.clear();
    for (int i = 0; i < len; ++i)
    {
        const double t = i / SR;
        if (std::fmod (t, 4.0) < 2.0) { const float v = 0.5f * (float) (std::sin (twoPi * 220 * t) + 0.3 * std::sin (twoPi * 660 * t)); in.setSample (0, i, v); in.setSample (1, i, v); }
    }
    auto measure = [&] (bool pro, float duck)
    {
        auto p = makeProc (pro, 120.0, SR, [&] (APP& q) { setParam (q, "feedback", 70); setParam (q, "mix", 100); setParam (q, "duck", duck); });
        std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
        run (*p, in, len);
        return std::pair<double, double> { rmsDb (wet, (int) (5.0 * SR), (int) (6.0 * SR)), rmsDb (wet, (int) (7.0 * SR), (int) (7.8 * SR)) };
    };
    const auto ref = measure (true, 0.0f);
    for (auto [pro, duck, want] : std::vector<std::tuple<bool, float, double>> { { false, 0.0f, 12.0 }, { true, 30.0f, 6.0 }, { true, 60.0f, 12.0 }, { true, 100.0f, 20.0 } })
    {
        const auto m = measure (pro, duck);
        const double playing = m.first - ref.first, gap = m.second - ref.second;
        std::printf ("  %s DUCK %3.0f%%: while playing %+5.1f dB, in the gap %+5.1f dB\n", pro ? "Pro " : "Lite", pro ? duck : ad::defDuck, playing, gap);
        check (std::abs (playing + want) < 1.5 && gap > -1.5, juce::String (pro ? "Pro" : "Lite (fixed)") + " ducking " + juce::String (playing, 1)
               + " dB while playing (want -" + juce::String (want, 0) + "), " + juce::String (gap, 1) + " dB in the gap");
    }
}

// ── 6. Auto throws ───────────────────────────────────────────────────────────
static float wordAt (double t, double f)
{
    if (t < 0 || t > 0.18) return 0.0f;
    const double env = std::min (1.0, t / 0.01) * std::min (1.0, (0.18 - t) / 0.03);
    double v = 0; for (int k = 1; k <= 6; ++k) v += std::sin (twoPi * f * k * t) / k;
    return (float) (0.35 * env * v);
}

static void throwTests()
{
    std::printf ("\n== Auto throws: only the last word of each phrase goes into the delay ==\n");
    const double bpm = 120.0; const int note = ad::n4; const double N = noteSamples (note, bpm);
    const double phraseAt[] = { 0.5, 3.5, 6.5 };
    const int words = 5; const double wordGap = 0.24;
    const int len = (int) (10 * SR);
    Buf in (2, len); in.clear();
    for (double p0 : phraseAt)
        for (int w = 0; w < words; ++w)
            for (int i = 0; i < (int) (0.2 * SR); ++i)
            {
                const double t = i / SR; const int idx = (int) ((p0 + w * wordGap) * SR) + i;
                const float v = wordAt (t, 180.0 + 40.0 * w);
                in.addSample (0, idx, v); in.addSample (1, idx, v);
            }
    auto p = makeProc (true, bpm, SR, [&] (APP& q) { setParam (q, "throws", 1); setParam (q, "time", (float) note); setParam (q, "feedback", 40); setParam (q, "mix", 100); setParam (q, "duck", 0); });
    std::vector<float> wet; wet.reserve ((size_t) len + 10); p->wetTrace = &wet;
    std::vector<juce::int64> tt; p->throwTrace = &tt;
    run (*p, in, len);
    check (p->throwCount.load() == 3, "3 phrases -> exactly 3 throws (got " + juce::String (p->throwCount.load()) + "; the 60 ms gaps between words don't count)");
    const double fire0 = phraseAt[0] + (words - 1) * wordGap + 0.18 + 0.2;
    float before = 0; for (int i = 0; i < (int) (fire0 * SR); ++i) before = juce::jmax (before, std::abs (wet[(size_t) i]));
    check (before < 1.0e-5f, "nothing echoes while the phrase is sung (wet peak " + juce::String (juce::Decibels::gainToDecibels (before, -200.0f), 1) + " dBFS)");
    for (size_t k = 0; k < tt.size() && k < 3; ++k)
    {
        const double lastWord = phraseAt[k] + (words - 1) * wordGap;
        const double src = (double) tt[k] / SR;
        check (src > lastWord - 0.03 && src < lastWord + 0.005, "throw " + juce::String ((int) k + 1) + " catches the last word only (starts " + juce::String ((src - lastWord) * 1000.0, 1) + " ms from it)");
    }
    // echoes of the last word land on the grid; echoes of word 4 never appear
    {
        const double lastWord = phraseAt[0] + (words - 1) * wordGap;
        float peak = 0; for (auto v : wet) peak = juce::jmax (peak, std::abs (v));
        int onset = -1; for (int i = (int) (fire0 * SR); i < (int) (3.4 * SR); ++i) if (std::abs (wet[(size_t) i]) > peak * 0.01f) { onset = i; break; }
        // the first audible echo is k whole notes after the word's start
        const double rel = (onset - lastWord * SR) / N;
        const int k = (int) std::round (rel);
        const double errMs = (onset - (lastWord * SR + k * N)) / SR * 1000.0;
        std::printf ("  first echo starts %.1f ms after the throw, = %d x 1/4 after the last word (%.2f ms off; word fades in over ~3 ms)\n", (onset / SR - fire0) * 1000.0, k, errMs);
        check (k >= 1 && errMs > -1.0 && errMs < 4.0, "the throw's first echo lands on the grid (" + juce::String (k) + " x 1/4 after the word, " + juce::String (errMs, 2) + " ms)");
        const double w4 = phraseAt[0] + (words - 2) * wordGap;
        double e4 = 0, e5 = 0;
        for (int i = (int) ((w4 + 2 * N / SR + 0.03) * SR); i < (int) ((w4 + 2 * N / SR + 0.15) * SR); ++i) e4 += (double) wet[(size_t) i] * wet[(size_t) i];
        for (int i = (int) ((lastWord + k * N / SR + 0.03) * SR); i < (int) ((lastWord + k * N / SR + 0.15) * SR); ++i) e5 += (double) wet[(size_t) i] * wet[(size_t) i];
        const double rel4 = 10 * std::log10 ((e4 + 1e-30) / (e5 + 1e-30));
        check (rel4 < -40.0, "no echo of the word before it (" + juce::String (rel4, 1) + " dB vs the last word's echo)");
    }
    // A sustained pad with no gaps never throws; throws OFF in Lite even if the param is on
    {
        auto q = makeProc (true, bpm, SR, [&] (APP& r) { setParam (r, "throws", 1); });
        Buf pad (2, (int) (6 * SR));
        for (int i = 0; i < pad.getNumSamples(); ++i) { const float v = 0.3f * (float) std::sin (twoPi * 220 * i / SR) * (0.7f + 0.3f * (float) std::sin (twoPi * 0.5 * i / SR)); pad.setSample (0, i, v); pad.setSample (1, i, v); }
        run (*q, pad);
        check (q->throwCount.load() == 0, "a held pad with no gaps never throws");
        auto lite = makeProc (false, bpm, SR, [&] (APP& r) { setParam (r, "throws", 1); });
        run (*lite, in, len);
        check (lite->throwCount.load() == 0, "Lite ignores AUTO THROWS (Pro feature)");
    }
    // Wet vocal: the same kind of phrases with a long reverb printed in (the level never falls far between
    // phrases). Words inside a phrase are legato (30 ms gaps), phrases are 0.25-0.4 s apart.
    {
        const double pGap[] = { 0.30, 0.25, 0.40, 0.30, 0.25, 0.35 };
        const int wlen = (int) (11 * SR);
        Buf dry (2, wlen); dry.clear();
        std::vector<double> lastWords;
        double t0 = 0.5;
        for (int k = 0; k < 6; ++k)
        {
            const int nw = 4 + k % 3;
            for (int w = 0; w < nw; ++w, t0 += 0.21)
                for (int i = 0; i < (int) (0.2 * SR); ++i)
                {
                    const float v = wordAt (i / SR, 160.0 + 30.0 * ((w + k) % 5));
                    dry.addSample (0, (int) (t0 * SR) + i, v); dry.addSample (1, (int) (t0 * SR) + i, v);
                }
            lastWords.push_back (t0 - 0.21);
            t0 += pGap[k] - 0.03;   // gap after the last word's end
        }
        auto wetVocal = [&] (float room, float wetDb)
        {
            juce::Reverb rv; juce::Reverb::Parameters rp; rp.roomSize = room; rp.damping = 0.25f; rp.wetLevel = 1.0f; rp.dryLevel = 0.0f; rp.width = 1.0f;
            rv.setParameters (rp); rv.setSampleRate (SR);
            Buf w (dry); rv.processStereo (w.getWritePointer (0), w.getWritePointer (1), wlen);
            // reverb level set by energy: wetDb = whole reverb return vs the dry vocal
            double eD = 0, eW = 0;
            for (int i = 0; i < wlen; ++i) { eD += (double) dry.getSample (0, i) * dry.getSample (0, i); eW += 0.5 * ((double) w.getSample (0, i) * w.getSample (0, i) + (double) w.getSample (1, i) * w.getSample (1, i)); }
            Buf out (dry); const float g = (float) (juce::Decibels::decibelsToGain ((double) wetDb) * std::sqrt (eD / (eW + 1e-30)));
            for (int c = 0; c < 2; ++c) out.addFrom (c, 0, w, c, 0, wlen, g);
            return out;
        };
        for (auto [room, wetDb, name] : { std::tuple<float, float, const char*> { 0.80f, -10.0f, "room, reverb -10 dB" },
                                          std::tuple<float, float, const char*> { 0.92f, -6.0f, "long hall, reverb -6 dB" } })
        {
            auto x = wetVocal (room, wetDb);
            auto q = makeProc (true, bpm, SR, [&] (APP& r) { setParam (r, "throws", 1); setParam (r, "time", (float) ad::n4); setParam (r, "mix", 100); setParam (r, "duck", 0); });
            std::vector<juce::int64> t3; q->throwTrace = &t3;
            run (*q, x, wlen);
            bool lastOnly = t3.size() == lastWords.size();
            for (size_t k = 0; lastOnly && k < t3.size(); ++k)
                lastOnly = std::abs ((double) t3[k] / SR - lastWords[k]) < 0.06;
            check (t3.size() == 6, juce::String ("wet vocal (") + name + "): 6 phrases -> " + juce::String ((int) t3.size()) + " throws");
            check (lastOnly, juce::String ("wet vocal (") + name + "): every throw starts on the phrase's last word");
        }
    }
    // A pad that swells and fades slowly (12 dB, 4 s cycle) never throws
    {
        Buf pad (2, (int) (10 * SR));
        for (int i = 0; i < pad.getNumSamples(); ++i)
        {
            const double t = i / SR, lfo = 0.5 + 0.5 * std::sin (twoPi * 0.25 * t);
            const float v = (float) (0.3 * std::pow (10.0, -12.0 * lfo / 20.0) * (std::sin (twoPi * 220 * t) + 0.5 * std::sin (twoPi * 277.2 * t) + 0.4 * std::sin (twoPi * 329.6 * t)));
            pad.setSample (0, i, v); pad.setSample (1, i, v);
        }
        auto q = makeProc (true, bpm, SR, [&] (APP& r) { setParam (r, "throws", 1); });
        run (*q, pad);
        check (q->throwCount.load() == 0, "a pad swelling and fading 12 dB never throws (got " + juce::String (q->throwCount.load()) + ")");
    }
    // Short delay: the word is older than one repeat -> it goes in a repeat later, still on the grid
    {
        auto q = makeProc (true, bpm, SR, [&] (APP& r) { setParam (r, "throws", 1); setParam (r, "time", (float) ad::n16); setParam (r, "mix", 100); setParam (r, "duck", 0); });
        std::vector<juce::int64> t2; q->throwTrace = &t2;
        run (*q, in, len);
        check (q->throwCount.load() == 3 && q->lastHitFirstRepeat.load() > 1, "1/16 throws: placed " + juce::String (q->lastHitFirstRepeat.load()) + " repeats on (the word is longer than one repeat)");
    }
}

// ── 7. Clicks ────────────────────────────────────────────────────────────────
struct HF { Biquad a = Biquad::hp (2500, 0.54, SR), b = Biquad::hp (2500, 1.31, SR); float operator() (float x) { return (float) b.p (a.p (x)); } };

static void clickTests()
{
    std::printf ("\n== Clicks when switching (200 Hz tone; HF > 2.5 kHz right after each switch vs steady) ==\n");
    struct Cat { const char* name; std::function<void (APP&, int)> change; };
    std::vector<Cat> cats = {
        { "style",       [] (APP& p, int k) { setParam (p, "style", (float) (k % ad::numStyles)); } },
        { "clean/pp",    [] (APP& p, int k) { setParam (p, "style", (float) (k % 2)); } },
        { "time steps",  [] (APP& p, int k) { setParam (p, "time", (float) ((k * 5) % (ad::numNotes + 1))); } },
        { "tape time",   [] (APP& p, int k) { setParam (p, "style", (float) ad::tape); setParam (p, "time", (float) ((k * 5) % (ad::numNotes + 1))); } },
        { "tempo",       [] (APP& p, int k) { p.forcedBpm = k % 2 ? 128.0 : 120.0; } },
        { "feedback",    [] (APP& p, int k) { setParam (p, "feedback", k % 2 ? 100.0f : 0.0f); } },
        { "mix",         [] (APP& p, int k) { setParam (p, "mix", k % 2 ? 0.0f : 100.0f); } },
        { "duck",        [] (APP& p, int k) { setParam (p, "duck", k % 2 ? 100.0f : 0.0f); } },
        { "tone",        [] (APP& p, int k) { setParam (p, "tone", k % 2 ? -100.0f : 100.0f); } },
        { "drive",       [] (APP& p, int k) { setParam (p, "drive", k % 2 ? 100.0f : 0.0f); } },
        { "width",       [] (APP& p, int k) { setParam (p, "width", k % 2 ? 0.0f : 150.0f); } },
        { "freeze",      [] (APP& p, int k) { setParam (p, "freeze", (float) (k % 2)); } },
        { "throws",      [] (APP& p, int k) { setParam (p, "throws", (float) (k % 2)); } },
        { "Pro on/off",  [] (APP& p, int k) { setPro (p, k % 2 ? hz::LicenceInfo::none : hz::LicenceInfo::pro); } },
    };
    const double secs = 6.0; const int total = (int) (secs * SR);
    Buf tone (2, total);
    for (int i = 0; i < total; ++i) { const float v = 0.3f * (float) std::sin (twoPi * 200.0 * i / SR); tone.setSample (0, i, v); tone.setSample (1, i, v); }
    std::printf ("%-14s %12s %12s %8s\n", "switch", "steady dB", "switch dB", "click");
    for (auto& cat : cats)
    {
        auto p = makeProc (true, 120.0, SR, [&] (APP& q)
        {
            setParam (q, "feedback", 50); setParam (q, "mix", 50);
            if (juce::String (cat.name) == "Pro on/off") { setParam (q, "style", (float) ad::tape); setParam (q, "drive", 60); setParam (q, "time", (float) ad::n8T); setParam (q, "tone", -50); }
        });
        HF hf; float steady = 0, sw = 0;
        const int every = (int) (0.25 * SR) / BS * BS, after = (int) (0.06 * SR);
        int k = 0, lastChange = -1000000;
        Buf blk (2, BS); juce::MidiBuffer m;
        for (int pos = 0; pos + BS <= total; pos += BS)
        {
            if (pos >= (int) (1.5 * SR) && pos % every == 0) { cat.change (*p, ++k); lastChange = pos; }
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, tone, c, pos, BS);
            p->processBlock (blk, m);
            for (int i = 0; i < BS; ++i)
            {
                const float y = std::abs (hf (blk.getSample (0, i)));
                if (pos + i < (int) (1.0 * SR)) continue;
                if (pos + i - lastChange < after) sw = juce::jmax (sw, y);
                else steady = juce::jmax (steady, y);
            }
        }
        const float sDb = juce::Decibels::gainToDecibels (steady, -150.0f), wDb = juce::Decibels::gainToDecibels (sw, -150.0f);
        const bool ok = wDb < sDb + 6.0f || wDb < -70.0f;
        std::printf ("%-14s %12.1f %12.1f %8s\n", cat.name, sDb, wDb, ok ? "none" : "CLICK");
        check (ok, juce::String ("no click: ") + cat.name);
    }
}

// ── 8. State ─────────────────────────────────────────────────────────────────
static Buf testLoop (double secs)
{
    const int n = (int) (secs * SR);
    Buf b (2, n); b.clear();
    juce::Random rnd (11);
    for (int s = 0; s * 0.25 < secs; ++s)
    {
        if (s % 4 == 3) continue;
        const int start = (int) (s * 0.25 * SR);
        const double f = 220.0 * std::pow (2.0, (s % 7) / 12.0);
        for (int i = 0; i < (int) (0.15 * SR) && start + i < n; ++i)
        {
            const double t = i / SR;
            const float v = (float) (0.4 * std::exp (-t / 0.05) * (std::sin (twoPi * f * t) + 0.3 * std::sin (twoPi * 2 * f * t)));
            b.addSample (0, start + i, v); b.addSample (1, start + i, v * 0.9f + 0.02f * (rnd.nextFloat() - 0.5f));
        }
    }
    return b;
}

static void stateTests()
{
    std::printf ("\n== Save -> new instance -> restore ==\n");
    auto a = makeProc (true);
    setParam (*a, "style", (float) ad::dub); setParam (*a, "time", (float) ad::n8T); setParam (*a, "feedback", 63); setParam (*a, "mix", 41);
    setParam (*a, "duck", 25); setParam (*a, "throws", 1); setParam (*a, "tone", -35); setParam (*a, "drive", 70); setParam (*a, "width", 130);
    juce::MemoryBlock mb; a->getStateInformation (mb);
    auto restored = [&]
    {
        auto b = std::make_unique<APP>(); setPro (*b, hz::LicenceInfo::pro); b->forcedBpm = 120;
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        b->setRateAndBufferSizeDetails (SR, BS); b->prepareToPlay (SR, BS);
        return b;
    };
    auto b = restored();
    bool same = true;
    for (auto* prm : a->getParameters()) if (std::abs (prm->getValue() - b->getParameters()[prm->getParameterIndex()]->getValue()) > 1e-6f) same = false;
    check (same, "every parameter survives save / restore");
    auto c = restored();
    for (auto* p : { b.get(), c.get() }) setParam (*p, "throws", 0);
    const auto loop = testLoop (4);
    auto o1 = run (*b, loop), o2 = run (*c, loop);
    float diff = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < o1.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (o1.getSample (ch, i) - o2.getSample (ch, i)));
    check (diff == 0.0f, "two restored instances sound identical");
    for (int i = 0; i < 2; ++i) { std::unique_ptr<juce::AudioProcessorEditor> ed (b->createEditor()); }
    check (true, "editor opens / closes twice without trouble");
}

// ── 9. Lite gating ───────────────────────────────────────────────────────────
static void liteTests()
{
    std::printf ("\n== Lite can't use Pro features (bit-identical to plain Lite) ==\n");
    const auto loop = testLoop (5);
    const int len = loop.getNumSamples();
    auto mk = [&] (int kind, std::function<void (APP&)> setup)
    {
        auto p = std::make_unique<APP>(); setPro (*p, kind); p->forcedBpm = 120;
        setParam (*p, "feedback", 55); setParam (*p, "mix", 45);
        if (setup) setup (*p);
        p->setRateAndBufferSizeDetails (SR, BS); p->prepareToPlay (SR, BS); return p;
    };
    auto proSettings = [] (APP& p, int style)
    {
        setParam (p, "style", (float) style); setParam (p, "time", (float) ad::n16T); setParam (p, "duck", 100); setParam (p, "throws", 1);
        setParam (p, "tone", -100); setParam (p, "drive", 100); setParam (p, "width", 0); setParam (p, "freeze", 1);
    };
    auto ref = mk (hz::LicenceInfo::none, nullptr);
    const auto refOut = run (*ref, loop, len);
    auto maxDiff = [&] (const Buf& o) { float d = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < len; ++i) d = juce::jmax (d, std::abs (o.getSample (c, i) - refOut.getSample (c, i))); return d; };
    for (int style : { (int) ad::clean, (int) ad::slapback, (int) ad::tape, (int) ad::dub, (int) ad::analog, (int) ad::lofi })
    {
        auto lite = mk (hz::LicenceInfo::none, [&] (APP& p) { proSettings (p, style); });
        const float d = maxDiff (run (*lite, loop, len));
        check (d == 0.0f, juce::String ("Lite + ") + ad::styleName (style) + " / 1/16T / duck 100 / throws / tone / drive / width 0 / freeze = plain Lite (max diff " + juce::String (d) + ")");
    }
    // automation mid-stream
    {
        auto lite = mk (hz::LicenceInfo::none, nullptr);
        Buf out (2, len), blk (2, BS); juce::MidiBuffer m;
        for (int pos = 0; pos < len; pos += BS)
        {
            const int n = juce::jmin (BS, len - pos);
            if (pos == BS * 40) proSettings (*lite, ad::tape);
            blk.setSize (2, n, false, false, true);
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, loop, c, pos, n);
            lite->processBlock (blk, m);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
        }
        check (maxDiff (out) == 0.0f, "Pro automation arriving mid-play changes nothing in Lite");
    }
    {
        auto pro = mk (hz::LicenceInfo::pro, [&] (APP& p) { proSettings (p, ad::tape); });
        juce::MemoryBlock mb; pro->getStateInformation (mb);
        for (int kind : { (int) hz::LicenceInfo::none, (int) hz::LicenceInfo::trialExpired, (int) hz::LicenceInfo::invalid })
        {
            auto lite = std::make_unique<APP>(); setPro (*lite, kind); lite->forcedBpm = 120;
            lite->setStateInformation (mb.getData(), (int) mb.getSize());
            lite->setRateAndBufferSizeDetails (SR, BS); lite->prepareToPlay (SR, BS);
            const char* names[] = { "none", "trial", "pro", "expired trial", "invalid key" };
            check (maxDiff (run (*lite, loop, len)) == 0.0f, juce::String ("Pro session opened with ") + names[kind] + " = plain Lite");
        }
        auto pro2 = mk (hz::LicenceInfo::pro, [&] (APP& p) { proSettings (p, ad::tape); setParam (p, "freeze", 0); setParam (p, "throws", 0); });
        check (maxDiff (run (*pro2, loop, len)) > 0.01f, "sanity: the same settings DO change the sound in Pro");
        auto trial = mk (hz::LicenceInfo::trial, [&] (APP& p) { proSettings (p, ad::tape); setParam (p, "freeze", 0); setParam (p, "throws", 0); });
        check (maxDiff (run (*trial, loop, len)) > 0.01f, "a running trial unlocks Pro");
        auto pp = mk (hz::LicenceInfo::none, [&] (APP& p) { setParam (p, "style", (float) ad::pingPong); });
        check (maxDiff (run (*pp, loop, len)) > 0.001f, "PING PONG works in Lite");
    }
}

// ── 10. UI ───────────────────────────────────────────────────────────────────
static void snapshots (const juce::File& outDir)
{
    std::printf ("\n== UI snapshots ==\n");
    const auto loop = testLoop (3);
    auto snap = [&] (const char* name, int licKind, const char* panel, std::function<void (APP&)> setup, double bpm = 124.0, double tailSecs = 0.6,
                     std::function<void (APP&)> afterPlay = nullptr)
    {
        auto p = std::make_unique<APP>(); setPro (*p, licKind); p->forcedBpm = bpm;
        if (setup) setup (*p);
        p->setRateAndBufferSizeDetails (SR, BS); p->prepareToPlay (SR, BS);
        run (*p, loop, (int) (2.6 * SR));
        if (afterPlay) afterPlay (*p);
        Buf quiet (2, (int) (tailSecs * SR)); quiet.clear(); run (*p, quiet);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        if (auto* t = dynamic_cast<juce::Timer*> (ed.get())) for (int i = 0; i < 20; ++i) t->timerCallback();
        if (panel != nullptr)
            for (auto* c : ed->getChildren())
                if (auto* pp = dynamic_cast<ProPanel*> (c)) pp->show (panel);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::FileOutputStream fo (outDir.getChildFile (name));
        fo.setPosition (0); fo.truncate();
        juce::PNGImageFormat().writeImageToStream (img, fo);
        std::printf ("  %s\n", name);
    };
    snap ("ui-1-lite.png", hz::LicenceInfo::none, nullptr, nullptr);
    snap ("ui-2-lite-pingpong-ducking.png", hz::LicenceInfo::none, nullptr, [] (APP& p) { setParam (p, "style", 1); setParam (p, "feedback", 60); }, 124.0, 0.0);
    snap ("ui-3-lite-pro-panel.png", hz::LicenceInfo::none, "TAPE IS A PRO STYLE", nullptr);
    snap ("ui-4-trial.png", hz::LicenceInfo::trial, nullptr, [] (APP& p) { setParam (p, "style", (float) ad::tape); });
    snap ("ui-5-trial-ended.png", hz::LicenceInfo::trialExpired, nullptr, [] (APP& p) { setParam (p, "style", (float) ad::dub); });
    snap ("ui-6-trial-ended-panel.png", hz::LicenceInfo::trialExpired, "", nullptr);
    snap ("ui-7-pro.png", hz::LicenceInfo::pro, nullptr, [] (APP& p)
          { setParam (p, "style", (float) ad::dub); setParam (p, "time", (float) ad::n8D); setParam (p, "feedback", 65); setParam (p, "duck", 80);
            setParam (p, "tone", -30); setParam (p, "drive", 40); setParam (p, "width", 120); setParam (p, "mix", 35); });
    snap ("ui-8-pro-throws.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "style", (float) ad::tape); setParam (p, "throws", 1); setParam (p, "time", (float) ad::n4); }, 124.0, 0.9);
    snap ("ui-9-pro-freeze-slapback-1-16.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "style", (float) ad::slapback); setParam (p, "time", (float) ad::n16); }, 128.0, 0.6,
          [] (APP& p) { setParam (p, "freeze", 1); });
    snap ("ui-10-pro-panel.png", hz::LicenceInfo::pro, "", nullptr);
}

// ── Demos on real audio ──────────────────────────────────────────────────────
static Buf loadStereo (const juce::File& f, double& rate)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return {};
    rate = r->sampleRate;
    const int len = (int) r->lengthInSamples;
    Buf x (2, len); r->read (&x, 0, len, 0, true, true);
    if (r->numChannels == 1) x.copyFrom (1, 0, x, 0, 0, len);
    return x;
}

static void demos (const juce::File& od, const juce::File& vocal, double vBpm, const juce::File& synth, double sBpm)
{
    od.createDirectory();
    struct Src { const char* tag; juce::File f; double bpm; };
    for (auto& s : { Src { "vocal", vocal, vBpm }, Src { "synth", synth, sBpm } })
    {
        double rate = 44100; auto x = loadStereo (s.f, rate);
        if (x.getNumSamples() == 0) { std::printf ("  missing %s\n", s.f.getFullPathName().toRawUTF8()); continue; }
        {   // skip leading silence (keep 0.5 s)
            int st = 0; while (st < x.getNumSamples() && std::abs (x.getSample (0, st)) < 1.0e-3f) ++st;
            st = juce::jmax (0, st - (int) (0.5 * rate));
            Buf t (2, x.getNumSamples() - st); for (int c = 0; c < 2; ++c) t.copyFrom (c, 0, x, c, st, t.getNumSamples());
            x = t;
        }
        const int tail = (int) (4 * rate);
        Buf in (2, x.getNumSamples() + tail); in.clear();
        for (int c = 0; c < 2; ++c) in.copyFrom (c, 0, x, c, 0, x.getNumSamples());
        writeWav (od.getChildFile (juce::String (s.tag) + "-0-DRY.wav"), in, rate);
        auto render = [&] (const juce::String& name, std::function<void (APP&)> setup)
        {
            auto p = std::make_unique<APP>(); setPro (*p, hz::LicenceInfo::pro); p->forcedBpm = s.bpm;
            if (setup) setup (*p);
            p->setRateAndBufferSizeDetails (rate, 512); p->prepareToPlay (rate, 512);
            auto out = run (*p, in, -1, 512);
            writeWav (od.getChildFile (juce::String (s.tag) + "-" + name + ".wav"), out, rate);
            std::printf ("  %s-%s.wav (throws: %d)\n", s.tag, name.toRawUTF8(), p->throwCount.load());
        };
        for (int st = 0; st < ad::numStyles; ++st)
            render (juce::String (st + 1) + "-" + juce::String (ad::styleName (st)).replace (" ", "-") + "-auto", [st] (APP& p) { setParam (p, "style", (float) st); });
        render ("8-CLEAN-ducking-OFF", [] (APP& p) { setParam (p, "duck", 0); setParam (p, "mix", 40); setParam (p, "feedback", 55); });
        render ("9-CLEAN-ducking-60-Lite", [] (APP& p) { setParam (p, "duck", 60); setParam (p, "mix", 40); setParam (p, "feedback", 55); });
        render ("10-CLEAN-ducking-100", [] (APP& p) { setParam (p, "duck", 100); setParam (p, "mix", 40); setParam (p, "feedback", 55); });
        render ("11-DUB-throws", [] (APP& p) { setParam (p, "style", (float) ad::dub); setParam (p, "throws", 1); setParam (p, "mix", 50); setParam (p, "feedback", 55); });
        render ("12-TAPE-throws-1-4", [] (APP& p) { setParam (p, "style", (float) ad::tape); setParam (p, "throws", 1); setParam (p, "time", (float) ad::n4); setParam (p, "mix", 50); setParam (p, "feedback", 50); });
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 3 && juce::String (argv[1]) == "scan")   // scan <wav> <bpm>: print where AUTO THROWS fire
    {
        double rate = 44100; auto x = loadStereo (juce::File (juce::String (argv[2])), rate);
        auto p = std::make_unique<APP>(); setPro (*p, hz::LicenceInfo::pro); p->forcedBpm = juce::String (argv[3]).getDoubleValue();
        setParam (*p, "throws", 1);
        p->setRateAndBufferSizeDetails (rate, 512); p->prepareToPlay (rate, 512);
        std::vector<juce::int64> tt; p->throwTrace = &tt;
        run (*p, x, -1, 512);
        std::printf ("throws: %d\n", p->throwCount.load());
        for (auto t : tt) std::printf ("  word from %.3f s\n", (double) t / rate);
        return 0;
    }
    if (argc > 6 && juce::String (argv[1]) == "demos")
    {
        demos (juce::File (juce::String (argv[2])), juce::File (juce::String (argv[3])), juce::String (argv[4]).getDoubleValue(),
               juce::File (juce::String (argv[5])), juce::String (argv[6]).getDoubleValue());
        return 0;
    }
    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();
    const bool quick = argc > 2 && juce::String (argv[2]) == "quick";

    timingTests();
    darkTests();
    if (! quick) boundTests();
    tailTests();
    duckTests();
    throwTests();
    if (! quick) clickTests();
    stateTests();
    liteTests();
    snapshots (outDir);

    std::printf ("\n== Licence keys ==\n");
    for (int a = 2; a < argc; ++a)
    {
        const juce::String arg (argv[a]);
        if (! arg.containsChar ('=')) continue;
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false);
        const auto info = hz::Licence::verify (arg.fromFirstOccurrenceOf ("=", false, false), "autodelaypro");
        const char* kinds[] = { "none", "trial", "pro", "trialExpired", "invalid" };
        std::printf ("  key %-14s -> %-12s daysLeft=%d\n", name.toRawUTF8(), kinds[(int) info.kind], info.daysLeft());
        hz::LicenceInfo::Kind want = hz::LicenceInfo::invalid;
        if (name == "pro" || name == "all") want = hz::LicenceInfo::pro;
        else if (name == "trial") want = hz::LicenceInfo::trial;
        else if (name == "expired") want = hz::LicenceInfo::trialExpired;
        check (info.kind == want, "key " + name + " -> " + kinds[(int) info.kind]);
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
