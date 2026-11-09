#pragma once

#include <cstdint>
#include <functional>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::spu2 {

void state(ps2::state::Stream &s);

void reset();
// The IOP queue that held the next sound tick was emptied (an IOP reset): the next register access schedules a new one.
void restart_ticks();
std::uint16_t *ram();
std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);
void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel);
void dma_stop(int core);
void dump_audio(const char *path);
void dump_voices();
void set_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

// How a voice's decoded samples become the chip's 48000 Hz samples when its pitch is not 0x1000 (Guitar Hero III's songs are 33075 Hz
// streams, pitch 0x0B06). PS2_SPU2_INTERP=linear is the straight line between two samples (the way before), anything else a
// windowed sinc over kInterpTaps samples (flat treble, no mirror images of the music above its own top frequency).
constexpr int kInterpTaps = 32;
// The sample between `at[0]` and `at[1]`, `frac` (12 bits) of the way: the sinc reads at[-15] to at[16]. For the tests.
std::int32_t interpolate(const std::int16_t *at, std::uint32_t frac, bool sinc);

}
