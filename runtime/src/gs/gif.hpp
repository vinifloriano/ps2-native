#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::gif {

void state(ps2::state::Stream &s);

void reset();
void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
// An XGKICK packet that did not end within VU1's data memory (kicked at `address`): end it, so the next kick starts a new packet
void end_runaway_kick(std::uint32_t address);
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc);
bool busy(int path);
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
// MSKPATH3 from VIF1, reported back to the EE in GIF_STAT.M3P
void set_path3_masked(bool masked);
// Forgets PATH3 data held back by the mask, on the thread that holds it
void drop_held();
// VIF1 has run out of work (its DMA ended), so an unmasked PATH3 has the GIF to itself
void vif1_idle();
// Channel 2's transfer would be over by now: PATH3 data still flowing behind the packet in flight reaches the GS (gif.cpp, transfer).
// Its event has a negative id: alarm events are numbered from hw::kEventCount up, so a new hw::Event would renumber saved alarms.
constexpr int kEventPath3Settle = -3;
void path3_settle();
// VIF1 at an MSKPATH3 window: true when PATH3 has nothing waiting to go through it, on the thread that holds PATH3 (vif.cpp).
bool path3_should_wait();
// PATH3 is part way through a packet and channel 2 has not sent the rest of it yet (VIF1 waits before it uses the GIF, vif.cpp).
bool path3_starved();
// VIF1 has worked through what it held while it waited at a window: an idle that came meanwhile takes effect now.
void vif1_drained();
// VIF1's DMA ended while VIF1 still waited at a window (its idle waits for vif1_drained).
bool vif1_idle_waiting();

}
