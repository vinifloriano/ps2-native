// No game data: VIF1 lets channel 2's PATH3 packets through MSKPATH3 windows in order with its own GIF traffic, even when the EE
// starts channel 2 just after channel 1. On the console VIF1 is still near the top of its list by then; here it used to run the whole
// list at once, so every window was empty and the textures came after the draws (Guitar Hero III: no text, black boxes in the menus,
// docs/apple/gh3-bakeoff.md). Each case writes FRAME_1 through PATH3 (channel 2's packet) and through VIF1's DIRECT: the value left
// shows which reached the GS last. Runs without the vector thread, the way --frames-dir and the tests run.
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "vif.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace gif = ps2::gif;
namespace vif = ps2::vif;

int failed = 0;
void check(bool ok, const char *what) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); }
}

constexpr std::uint32_t kMask = 0x06008000u, kUnmask = 0x06000000u, kNop = 0, kDirect2 = 0x50000002u;
constexpr std::uint64_t kFrame1 = 0x4C;  // A+D address of FRAME_1

std::uint64_t frame() { return ps2::gs::g_state.ctx[0].frame; }

void push_qword(std::vector<std::uint32_t> &w, std::uint64_t lo, std::uint64_t hi) {
    w.push_back(static_cast<std::uint32_t>(lo));
    w.push_back(static_cast<std::uint32_t>(lo >> 32));
    w.push_back(static_cast<std::uint32_t>(hi));
    w.push_back(static_cast<std::uint32_t>(hi >> 32));
}

// A one-register A+D GIF packet that sets FRAME_1: its tag (NLOOP 1, EOP, one A+D descriptor) and its data.
void packet(std::vector<std::uint32_t> &w, std::uint64_t value) {
    push_qword(w, 1u | (1ull << 15) | (1ull << 60), 0xE);
    push_qword(w, value, kFrame1);
}

// VIF1: mask, a window (unmask, mask), then DIRECT with FRAME_1 = value, and the mask left down at the end.
std::vector<std::uint32_t> vif1_list(std::uint64_t value) {
    std::vector<std::uint32_t> w{kMask, kUnmask, kMask, kDirect2};  // DIRECT's data starts on the next quadword
    packet(w, value);
    w.insert(w.end(), {kUnmask, kNop, kNop, kNop});
    return w;
}

// The list as one DMA transfer: the words, then the end of channel 1 (dmac.cpp calls vif1_idle there).
void send_vif1(const std::vector<std::uint32_t> &w) {
    vif::transfer_words(1, w.data(), static_cast<std::uint32_t>(w.size()));
    gif::vif1_idle();
}
void send_path3(const std::vector<std::uint32_t> &w) { gif::transfer(3, reinterpret_cast<const std::uint8_t *>(w.data()), static_cast<std::uint32_t>(w.size() / 4)); }

void start() {
    gif::render_reset();
    gif::reset();
    vif::reset();
    gif::set_path3_masked(false);
}
}

int main() {
    // 1. Channel 1 starts first, channel 2 a moment later: its packet goes through the window, before VIF1's DIRECT.
    start();
    send_vif1(vif1_list(0xBBBB));
    check(vif::path3_waiting(), "VIF1 waits at a window with nothing from channel 2 yet");
    check(frame() != 0xBBBB, "VIF1's DIRECT has not reached the GS while VIF1 waits");
    std::vector<std::uint32_t> p3;
    packet(p3, 0xAAAA);
    send_path3(p3);
    check(!vif::path3_waiting(), "channel 2's packet lets VIF1 go on to the end of its list");
    check(frame() == 0xBBBB, "channel 2's packet went through the window, ahead of VIF1's DIRECT");

    // 2. Channel 2's packet comes in two blocks: the GIF stays with it, so VIF1's DIRECT waits for the second block.
    start();
    send_vif1(vif1_list(0xDDDD));
    std::vector<std::uint32_t> whole, first, second;
    packet(whole, 0xCCCC);
    first.assign(whole.begin(), whole.begin() + 4);
    second.assign(whole.begin() + 4, whole.end());
    send_path3(first);
    check(vif::path3_waiting(), "VIF1 waits while channel 2's packet is part way through");
    check(frame() != 0xDDDD, "VIF1's DIRECT waits behind the unfinished PATH3 packet");
    send_path3(second);
    check(!vif::path3_waiting(), "the rest of the packet lets VIF1 go on to the end of its list");
    check(frame() == 0xDDDD, "the whole PATH3 packet reached the GS before VIF1's DIRECT");

    // 3. No channel 2 comes: the wait ends (the event of kEventPath3Wait, called here directly) and VIF1 finishes its list.
    start();
    send_vif1(vif1_list(0xEEEE));
    check(vif::path3_waiting(), "VIF1 waits at an empty window");
    vif::path3_resume();
    check(!vif::path3_waiting() && frame() == 0xEEEE, "VIF1 goes on when the wait ends without channel 2");

    std::printf("VIF1 and PATH3 windows: %d failed\n", failed);
    return failed ? 1 : 0;
}
