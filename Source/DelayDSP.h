#pragma once
// AutoDelay Pro: small DSP building blocks + the sound of each style.
#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>
#include "Features.h"

namespace ad
{
inline constexpr double pi = 3.14159265358979323846;

// What each style does inside the feedback loop. Every repeat passes through all of it again,
// so each one comes back a little darker (LP), thinner (HP), softer (saturation) and, for the
// tape-type styles, slightly bent in pitch (wow / flutter on the read head).
struct StyleModel
{
    float lpHz, hpHz, lpQ, hpQ;
    float sat, satBias;          // saturation amount 0..1, asymmetry (even harmonics)
    float wowMs, wowHz, flutMs, flutHz;
    float fbScale;               // feedback knob range (slapback = few repeats)
    float pingPong;              // 1 = mono in, repeats bounce L / R
    float stereoPhase;           // wow phase offset between L and R (radians): stereo movement
    bool  glide;                 // time changes bend the pitch like a tape machine (else crossfade)
};

inline const StyleModel& model (int s)
{
    static const StyleModel m[numStyles] = {
        //   lp      hp    lpQ   hpQ   sat   bias   wow   wowHz flut  flutHz fb    pp    stPh  glide
        { 11000.f,  90.f, 0.62f, 0.60f, 0.05f, 0.00f, 0.00f, 0.0f, 0.00f, 0.0f, 1.00f, 0.0f, 0.0f, false },  // CLEAN
        {  9000.f, 110.f, 0.62f, 0.60f, 0.08f, 0.00f, 0.00f, 0.0f, 0.00f, 0.0f, 1.00f, 1.0f, 0.0f, false },  // PING PONG
        {  6500.f, 140.f, 0.62f, 0.60f, 0.30f, 0.05f, 0.00f, 0.0f, 0.00f, 0.0f, 0.55f, 0.0f, 0.0f, false },  // SLAPBACK
        {  5200.f, 110.f, 0.66f, 0.60f, 0.45f, 0.10f, 0.32f, 0.55f, 0.05f, 6.3f, 1.00f, 0.0f, 0.6f, true  },  // TAPE
        {  2800.f, 280.f, 0.85f, 0.70f, 0.60f, 0.12f, 0.22f, 0.40f, 0.04f, 5.1f, 1.00f, 0.0f, 0.9f, true  },  // DUB
        {  3600.f,  70.f, 0.70f, 0.60f, 0.30f, 0.06f, 0.55f, 0.80f, 0.00f, 0.0f, 1.00f, 0.0f, 3.14159f, true },  // ANALOG (BBD chorus)
        {  3000.f, 450.f, 0.90f, 0.80f, 0.60f, 0.15f, 0.14f, 0.90f, 0.12f, 9.0f, 1.00f, 0.0f, 0.0f, true  },  // LO-FI
    };
    return m[s >= 0 && s < numStyles ? s : 0];
}

// Feedback knob (0..1) -> loop gain. 100 % stays under 1 (long tails that still die away), the saturation catches the rest.
inline float loopGain (float fb, int style) { return 0.97f * std::pow (std::clamp (fb, 0.0f, 1.0f), 0.8f) * model (style).fbScale; }

// TONE (-1..1) and DRIVE (0..1) move the style's own filters.
inline float lpFor (int s, float tone, float drive) { return std::clamp (model (s).lpHz * std::exp2 (1.5f * tone - 0.6f * drive), 900.0f, 18000.0f); }
inline float hpFor (int s, float tone, float)       { return std::clamp (model (s).hpHz * std::exp2 (1.0f * tone), 20.0f, 1500.0f); }
inline float satFor (int s, float drive)            { return std::clamp (model (s).sat + 0.85f * drive, 0.0f, 1.0f); }

// Zero-delay-feedback state variable filter (Simper / Zavalishin). Smooth under modulation.
struct SVF
{
    float g = 0, k = 1.4f, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set (float fc, float q, double sr)
    {
        g = (float) std::tan (pi * std::min ((double) fc, 0.49 * sr) / sr);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0; }
    // returns lowpass; hp via the other output
    inline void tick (float v0, float& lp, float& hp)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        lp = v2; hp = v0 - k * v1 - v2;
    }
};

// Group delay (samples) of the loop's LP + HP at frequency f, so repeats land exactly on the grid:
// the read head is moved earlier by this much. (A TPT SVF = bilinear transform of the analogue SVF,
// so the RBJ cookbook biquads with the same fc / Q have the same response.)
inline double groupDelay (double lpHz, double lpQ, double hpHz, double hpQ, double f, double sr)
{
    using C = std::complex<double>;
    auto gd = [&] (const double b[3], const double a[3])
    {
        const double w = 2 * pi * f / sr;
        const C z1 = std::polar (1.0, -w), z2 = z1 * z1;
        const C B = b[0] + b[1] * z1 + b[2] * z2, Bd = b[1] * z1 + 2.0 * b[2] * z2;
        const C A = a[0] + a[1] * z1 + a[2] * z2, Ad = a[1] * z1 + 2.0 * a[2] * z2;
        return (Bd / B).real() - (Ad / A).real();
    };
    auto coeffs = [&] (bool lp, double fc, double q, double b[3], double a[3])
    {
        const double w0 = 2 * pi * std::min (fc, 0.49 * sr) / sr, al = std::sin (w0) / (2 * q), c = std::cos (w0);
        a[0] = 1 + al; a[1] = -2 * c; a[2] = 1 - al;
        if (lp) { b[0] = (1 - c) / 2; b[1] = 1 - c; b[2] = (1 - c) / 2; }
        else    { b[0] = (1 + c) / 2; b[1] = -(1 + c); b[2] = (1 + c) / 2; }
    };
    double b[3], a[3], tot = 0;
    coeffs (true, lpHz, lpQ, b, a);  tot += gd (b, a);
    coeffs (false, hpHz, hpQ, b, a); tot += gd (b, a);
    return tot;
}

// |H| of the loop filters at f: used to draw how loud each repeat will be.
inline double loopMag (double lpHz, double lpQ, double hpHz, double hpQ, double f)
{
    const double rl = f / lpHz, rh = f / hpHz;
    const double lp = 1.0 / std::sqrt ((1 - rl * rl) * (1 - rl * rl) + (rl / lpQ) * (rl / lpQ));
    const double hp = rh * rh / std::sqrt ((1 - rh * rh) * (1 - rh * rh) + (rh / hpQ) * (rh / hpQ));
    return lp * hp;
}

// Saturation: slope 1 at zero (quiet repeats untouched), loud repeats get rounded and softer.
inline float loopSat (float x, float amt, float bias)
{
    if (amt > 1.0e-4f)
    {
        const float k = 0.25f + 1.6f * amt, b = bias;
        const float tb = std::tanh (k * b);
        x = (std::tanh (k * (x + b)) - tb) / (k * (1.0f - tb * tb));
    }
    // hard safety: never past +-1 inside the loop
    const float a = std::abs (x), t = 0.8f;
    if (a > t) x = std::copysign (t + (1.0f - t) * std::tanh ((a - t) / (1.0f - t)), x);
    return x;
}

// Cubic Hermite (Catmull-Rom) read from a ring buffer: `pos` samples behind write index w.
inline float readHermite (const float* buf, int size, int w, double delay)
{
    const double rp = (double) w - delay;
    double fl = std::floor (rp);
    const float t = (float) (rp - fl);
    int i1 = (int) fl % size; if (i1 < 0) i1 += size;
    int i0 = i1 - 1; if (i0 < 0) i0 += size;
    int i2 = i1 + 1; if (i2 >= size) i2 -= size;
    int i3 = i2 + 1; if (i3 >= size) i3 -= size;
    const float y0 = buf[i0], y1 = buf[i1], y2 = buf[i2], y3 = buf[i3];
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

// First-order allpass (stereo decorrelation for WIDTH above 100 %).
struct AP1
{
    float a = 0, x1 = 0, y1 = 0;
    inline float tick (float x) { const float y = -a * x + x1 + a * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0; }
};
} // namespace ad
