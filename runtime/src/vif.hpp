#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::vif {

void state(ps2::state::Stream &s);

void reset();
void reset_vu_side();
// The ITOP VU0 sees (XITOP): the one VIF0 latched at its last MSCAL. VU0 has its own, apart from VU1's TOP and ITOP.
std::uint32_t itop(int unit);
void stats();
void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_words(int unit, const std::uint32_t *words, std::uint32_t count);
bool stalled(int unit);
std::uint32_t read(int unit, std::uint32_t reg);
void write(int unit, std::uint32_t reg, std::uint32_t value);
// VIF1 holds at an empty MSKPATH3 window until channel 2's data comes (vif.cpp, execute); path3_resume lets it go on, on the thread
// that holds PATH3 (gif.cpp). The event that ends the wait without the vector thread has a negative id, like the other new ones.
constexpr int kEventPath3Wait = -4;
bool path3_waiting();
void path3_resume();
// VIF1's DMA has ended: a wait at a window with nothing after it that uses the GIF ends (true), since nothing follows it.
bool path3_window_is_last();

}
