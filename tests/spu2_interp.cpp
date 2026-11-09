// No game data: how an SPU2 voice's samples are resampled to 48000 Hz. Guitar Hero III plays its songs as 33075 Hz streams (pitch
// 0x0B06), so every song goes through this. A sine of the stream's rate is read between its samples at many fractions; the result is
// compared with the true sine there. The straight line between two samples loses treble and adds mirror images (large errors at
// 8 and 12 kHz); the windowed sinc must stay close at those frequencies, pass a steady level exactly, and give the sample itself
// back at fraction 0 for a slow sine.
#include "iop/spu2.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
namespace spu2 = ps2::iop::spu2;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 33075.0;  // Guitar Hero III's song streams
constexpr double kAmplitude = 12000.0;

// RMS error, as a fraction of the sine's amplitude, of reading a `hz` sine between its samples
double error(double hz, bool sinc) {
    std::vector<std::int16_t> s(400);
    for (std::size_t n = 0; n < s.size(); ++n) s[n] = static_cast<std::int16_t>(std::lround(kAmplitude * std::sin(2.0 * kPi * hz * n / kRate)));
    double sum = 0.0;
    int count = 0;
    for (int n = 50; n < 350; n += 3)
        for (std::uint32_t frac = 0; frac < 0x1000; frac += 0x1000 / 16 + 7) {
            const double t = n + frac / 4096.0;
            const double want = kAmplitude * std::sin(2.0 * kPi * hz * t / kRate);
            const double got = spu2::interpolate(&s[n], frac, sinc);
            sum += (got - want) * (got - want);
            ++count;
        }
    return std::sqrt(sum / count) / kAmplitude;
}
}

int main() {
    const double hz[] = {440.0, 4000.0, 8000.0, 12000.0};
    for (double f : hz) {
        const double line = error(f, false), sinc = error(f, true);
        std::printf("%6.0f Hz: straight line %.4f, sinc %.4f (of the amplitude)\n", f, line, sinc);
        check(sinc <= line + 1e-4, "the sinc is no worse than the straight line");
    }
    check(error(8000.0, true) < 0.01, "an 8 kHz sine read between its samples stays within 1% (the straight line is far off)");
    check(error(12000.0, true) < 0.03, "a 12 kHz sine stays within 3%");
    check(error(8000.0, false) > 0.05, "the straight line is off by more than 5% at 8 kHz (what the sinc fixes)");

    std::vector<std::int16_t> steady(64, 5000);
    bool flat = true;
    for (std::uint32_t frac = 0; frac < 0x1000; frac += 37) flat = flat && spu2::interpolate(&steady[32], frac, true) == 5000;
    check(flat, "a steady level passes through the sinc unchanged at every fraction");

    std::vector<std::int16_t> slow(64);
    for (std::size_t n = 0; n < slow.size(); ++n) slow[n] = static_cast<std::int16_t>(std::lround(kAmplitude * std::sin(2.0 * kPi * 100.0 * n / kRate)));
    bool same = true;
    for (int n = 20; n < 40; ++n) same = same && std::abs(spu2::interpolate(&slow[n], 0, true) - slow[n]) <= 1;
    check(same, "at fraction 0 a slow sine's own sample comes back");

    if (failed) return 1;
    std::printf("ok\n");
    return 0;
}
