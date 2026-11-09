// No game data: an EE command packet for a guest IOP handler (cid below 0x80000000, registered with sceSifAddCmdHandler on the IOP)
// reaches that handler only when the EE's SIF DMA that carries it is over, as on the console, where the IOP's SIF1 interrupt comes at
// the end of the transfer. Guitar Hero III's async file layer sends a request, waits for sceSifDmaStat to say done and only then
// reads its request id back from a slot that the IOP module's answer clears: an answer before that made it open its file with id -1
// and wait forever (docs/apple/gh3-bakeoff.md). The runtime's own system commands (cid 0x8000xxxx) are still taken at once
// (tests/sif_unhandled_cmd.cpp covers those).
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace iop = ps2::iop;
namespace kernel = ps2::iop::kernel;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}

constexpr std::uint32_t kCid = 0x00000001u;  // a user command, as Neversoft's FILEIO.IRX registers
constexpr std::uint32_t kPacket = 0x00120000, kList = 0x00121000;
constexpr std::uint32_t kPacketSize = 0x70;  // GH3's request size

// A synthetic IOP module importing sifcmd 10 (sceSifAddCmdHandler), 11, 12 and 13, as in tests/sif_unhandled_cmd.cpp.
std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x180);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, 0x80u); put(104, 0x80u);
    put(0x84, 0x60u); put(0x8C, 0x80u);
    put(0x100, 0x41E00000u); put(0x108, 0x101u);
    std::memcpy(elf.data() + 0x10C, "sifcmd", 6);
    for (unsigned i = 0; i < 4; ++i) {
        put(0x114 + i * 8, 0x03E00008u);
        put(0x118 + i * 8, 0x24000000u | (10u + i));
    }
    put(0x160, 0x03E00008u); // module start: jr ra; nop
    return elf;
}

constexpr std::uint32_t kData = 0x00122000, kDataSize = 0x800, kIopData = 0x00030000;  // extra data, more than SIF1's FIFO

// The EE sends a 0x70-byte packet with request id `opt` in the header to the IOP's command buffer. With `data`, 2 KiB of extra data
// go ahead of it in the same DMA list (as ps2sdk's _SifSendCmd sends them): more than the 512-byte FIFO, so the transfer takes time.
std::uint32_t ee_send(std::uint32_t opt, std::uint32_t fill, bool data = true) {
    for (std::uint32_t i = 0; i < kPacketSize; i += 4) ps2::st32(kPacket + i, fill + i);
    ps2::st32(kPacket, kPacketSize | (data ? kDataSize << 8 : 0)); ps2::st32(kPacket + 4, data ? kIopData : 0);
    ps2::st32(kPacket + 8, kCid); ps2::st32(kPacket + 12, opt);
    std::uint32_t at = kList;
    if (data) {
        ps2::st32(at, kData); ps2::st32(at + 4, kIopData); ps2::st32(at + 8, kDataSize); ps2::st32(at + 12, 0);
        at += 16;
    }
    ps2::st32(at, kPacket); ps2::st32(at + 4, sif::get_reg(2)); ps2::st32(at + 8, kPacketSize); ps2::st32(at + 12, 0x44);
    return sif::set_dma(kList, data ? 2 : 1);
}

void spend_ee_cycles(std::int32_t n) { ps2::g_cpu.budget -= n; }  // now() counts the budget the EE has used
}

int main() {
    iop::reset();
    iop::run(8192);

    const int id = iop::load_module_buffer(module(), "sifcmd-timing-test.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    const auto base = kernel::modules().at(id).base;
    iop::run(8192);
    const std::uint32_t add_handler = iop::ram32(base + 20);
    check(add_handler >> 26 == iop::kHleOpcode, "sceSifAddCmdHandler links to its HLE handler");
    int calls = 0;
    std::uint32_t seen_opt = 0, seen_word = 0;
    const std::uint32_t handler = kernel::hle_stub([&](iop::Cpu &c) {
        ++calls;
        seen_opt = iop::ram32(c.r[4] + 12);
        seen_word = iop::ram32(c.r[4] + 0x40);
    });
    iop::Cpu c{};
    c.r[4] = kCid; c.r[5] = handler; c.r[6] = 0; c.r[29] = 0x18200;
    iop::call_hle(c, add_handler & 0x03FFFFFFu);

    // 1. Sent: the EE's DmaStat says busy and the handler has not run yet.
    const std::uint32_t dma = ee_send(5, 0x55550000u);
    check(calls == 0, "the handler does not run before the EE's DMA is over");
    check(sif::dma_stat(dma) >= 0, "sceSifDmaStat says the transfer is still going");
    const std::uint64_t due = ps2::scheduled_at(sif::kEventSifCommand);
    check(due > ps2::now() && due != ~0ull, "the packet's delivery is scheduled for the end of the transfer");

    // 2. The EE reuses its buffer once the transfer is over (as GH3 does for its next request): the IOP still gets what was sent.
    spend_ee_cycles(static_cast<std::int32_t>(due - ps2::now()));
    check(sif::dma_stat(dma) < 0, "sceSifDmaStat says done at the end of the transfer");
    for (std::uint32_t i = 0; i < kPacketSize; i += 4) ps2::st32(kPacket + i, 0xDEAD0000u + i);
    sif::deliver_commands();
    check(calls == 1, "the handler runs once when the transfer is over");
    check(seen_opt == 5, "the handler sees the request id of the packet that was sent");
    check(seen_word == 0x55550040u, "the handler sees the packet as it was when the transfer started");
    check(iop::ram32(kIopData) == ps2::ld32(kData), "the extra data is in IOP memory when the handler runs");

    // 3. Two packets back to back keep their order and each waits for its own transfer.
    ee_send(6, 0x66660000u);
    const std::uint32_t second = ee_send(7, 0x77770000u);
    sif::deliver_commands();
    check(calls == 1, "nothing is handed over before its transfer is over");
    spend_ee_cycles(static_cast<std::int32_t>(ps2::scheduled_at(sif::kEventSifCommand) - ps2::now()));
    sif::deliver_commands();
    check(calls == 2 && seen_opt == 6, "the first of two packets comes first");
    check(sif::dma_stat(second) >= 0, "the second transfer is still going");
    spend_ee_cycles(static_cast<std::int32_t>(ps2::scheduled_at(sif::kEventSifCommand) - ps2::now()));
    sif::deliver_commands();
    check(calls == 3 && seen_opt == 7, "the second packet comes at the end of its own transfer");

    // 4. A packet alone fits in SIF1's FIFO: the DMA is over as soon as it starts, so sceSifDmaStat says done at once (the EE then
    //    reads its request id back before the IOP can answer, Guitar Hero III's file layer) and the packet is handed over right away.
    const std::uint32_t small = ee_send(8, 0x88880000u, false);
    check(sif::dma_stat(small) < 0, "sceSifDmaStat says done at once for a packet that fits in the FIFO");
    check(ps2::scheduled_at(sif::kEventSifCommand) <= ps2::now(), "a packet that fits in the FIFO is due at once");
    sif::deliver_commands();
    check(calls == 4 && seen_opt == 8, "a packet that fits in the FIFO reaches the handler at once");

    std::printf("SIF user command timing: %d failed\n", failed);
    return failed ? 1 : 0;
}
