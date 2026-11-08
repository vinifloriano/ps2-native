// No game data: ENVX of an SPU2 voice that was keyed on a few samples ago. The console starts the attack at the sample after KON, so
// a driver that reads ENVX shortly after keying a voice on sees it rising. The runtime renders the voices in batches of 32 samples
// and only starts a keyed-on voice at the next batch; before that ENVX showed the level of before the KON (0), and Guitar Hero III's
// MSTREAM.IRX, which reads ENVX to tell whether a stream's voice has ended, stopped the second voice of its intro film's stereo pair
// so the film never started (docs/apple/gh3-bakeoff.md).
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/spu2.hpp"

#include <cstdio>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace spu2 = ps2::iop::spu2;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kSpu2 = 0x1F900000u;
constexpr std::uint32_t kVoice = 1;                                  // core 0, voice 1
constexpr std::uint32_t kAdsr1 = kSpu2 + kVoice * 0x10u + 0x6u, kEnvx = kSpu2 + kVoice * 0x10u + 0xAu;
constexpr std::uint32_t kKonLow = kSpu2 + 0x1A0u;
constexpr std::uint64_t kCyclesPerSample = 768;                      // IOP cycles per 48 kHz sample
constexpr std::uint64_t kEeCyclesPerSample = kCyclesPerSample * 8;   // iop::run counts EE cycles, eight per IOP cycle
constexpr std::uint32_t kAttackRate = 0x20;                          // linear, +56 per sample (7 << (11 - 8))
}

int main() {
    iop::reset();
    iop::run(8192);

    spu2::write(kAdsr1, kAttackRate << 8, 2);  // starts the batch clock: the next batch is 32 samples from here
    spu2::write(kKonLow, 1u << kVoice, 2);
    check(spu2::read(kEnvx, 2) == 0, "ENVX is 0 at the KON itself");

    const std::uint64_t keyed = kernel::now();
    iop::run(10 * kEeCyclesPerSample);  // well before the next batch
    const std::uint64_t samples = (kernel::now() - keyed) / kCyclesPerSample;
    const std::uint32_t level = spu2::read(kEnvx, 2);
    check(samples >= 5 && samples < 32, "the test reads ENVX a few samples after the KON, before the next batch");
    check(level > 0, "ENVX is rising a few samples after the KON");
    check(level == samples * 56u, "ENVX follows the attack rate for the samples since the KON");

    iop::run(40 * kEeCyclesPerSample);  // past the next batch, which starts the voice
    check(spu2::read(kEnvx, 2) > 0, "ENVX stays above 0 once the batch has started the voice");

    std::printf("SPU2 ENVX after KON: %d failed\n", failed);
    return failed ? 1 : 0;
}
