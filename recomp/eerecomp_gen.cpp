#include "eerecomp_gen.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace recomp {

Entries read_entries(const std::string &path) {
    Entries entries;
    std::ifstream file(ps2::os::path(path));
    std::string token;
    while (file >> token) {
        if (token[0] == '#') { std::getline(file, token); continue; }
        const auto colon = token.find(':');
        if (colon == std::string::npos)
            entries.main.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
        else
            entries.modules[token.substr(0, colon)].push_back(static_cast<std::uint32_t>(std::stoul(token.substr(colon + 1), nullptr, 16)));
    }
    return entries;
}

std::set<std::uint32_t> dynamic_sites(const Links &links) {
    std::set<std::uint32_t> sites;
    for (const auto &[at, link] : links)
        if (link.kind == Fix::Jump && link.external) sites.insert(at);
    return sites;
}

std::vector<std::uint32_t> window_entries(const Image &image, const Program &program, std::uint32_t base, std::uint32_t size) {
    std::vector<std::uint32_t> seeds;
    for (std::uint32_t pc = program.base; pc < program.end; pc += 4) {
        if (!program.is_code(pc)) continue;
        const Insn i = decode(*image.word(pc), pc);
        const Flow f = flow(i.op);
        if (f != Flow::Jump && f != Flow::Call) continue;
        const std::uint32_t t = i.jump_target();
        if (t - base < size) seeds.push_back(t);
    }
    return seeds;
}

// Zero words are left out: a zero is a NOP to the analysis, so the zeros after a module's end (its .bss, free memory) count as code,
// and the game later writes data there. Grand Theft Auto: San Andreas's MPEG.NM ends at 0x89D7C0 in a window that reaches 0x8A0000;
// its image never matched again once the game used those words. Swapping modules changes the real code words, which stay checked.
std::vector<std::pair<std::uint32_t, std::uint32_t>> checks_for_window(const Snr2Module &o, const Program &program) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks;
    for (std::uint32_t at = 0; at + 4 <= o.size; at += 4) {
        if (!program.is_code(o.base + at)) continue;
        std::uint32_t word = 0;
        std::memcpy(&word, o.bytes.data() + at, 4);
        if (word == 0) continue;
        checks.emplace_back(at, word);
    }
    return checks;
}

HeadChecks entry_head_checks(const Snr2Module &o, const Program &program) {
    const auto word_at = [&](std::uint32_t at) {
        std::uint32_t word = 0;
        std::memcpy(&word, o.bytes.data() + at, 4);
        return word;
    };
    // The words that belong to the first words of an entry (up to its return and the delay slot of that, at most 16).
    const auto head = [&](std::uint32_t entry) {
        std::vector<std::uint32_t> offsets;
        bool after_return = false;
        for (std::uint32_t k = 0; k < 16; ++k) {
            const std::uint32_t at = entry - o.base + 4 * k;
            if (at + 4 > o.size || !program.is_code(o.base + at)) break;
            offsets.push_back(at);
            if (after_return) break;                  // the delay slot of the return
            after_return = word_at(at) == 0x03E00008u;       // jr ra
        }
        return offsets;
    };
    const auto inside = [&](std::uint32_t entry) { return program.is_code(entry) && entry >= o.base && entry - o.base < o.size; };
    std::set<std::uint32_t> offsets;
    for (const std::uint32_t entry : program.entries)
        if (inside(entry) && !program.weak.count(entry))
            for (const std::uint32_t at : head(entry)) offsets.insert(at);
    HeadChecks checks;
    checks.hard.reserve(offsets.size());
    for (const std::uint32_t at : offsets) checks.hard.emplace_back(at, word_at(at));
    for (const std::uint32_t entry : program.weak) {
        if (!inside(entry)) continue;
        std::vector<std::uint32_t> mine;
        for (const std::uint32_t at : head(entry))
            if (!offsets.count(at)) mine.push_back(at);   // words a hard entry checks anyway are proven there
        if (mine.empty()) continue;
        checks.soft.push_back(entry - o.base);
        checks.soft.push_back(static_cast<std::uint32_t>(mine.size()));
        for (const std::uint32_t at : mine) {
            checks.soft.push_back(at);
            checks.soft.push_back(word_at(at));
        }
    }
    return checks;
}

// ---------------------------------------------------------------------------------------------------------------- capturing

std::vector<std::uint32_t> code_windows(const std::vector<std::uint8_t> &elf) {
    std::vector<std::uint32_t> out;
    if (elf.size() < 52) return out;
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 <= elf.size()) std::memcpy(&v, elf.data() + at, 4);
        return v;
    };
    const auto u16 = [&](std::size_t at) {
        std::uint16_t v = 0;
        if (at + 2 <= elf.size()) std::memcpy(&v, elf.data() + at, 2);
        return v;
    };
    const std::uint32_t phoff = u32(28);
    const std::uint16_t phentsize = u16(42), phnum = u16(44);
    // Segments that begin at the same address are alternatives for one window, not windows of their own: Grand Theft Auto: San
    // Andreas has one per .NM overlay, all eight at 0x88D880. Moving each to the next 16 KiB made eight windows that each captured
    // a slice of whichever overlay was loaded, more images than the folder keeps (scripts/windows.py does the same).
    std::vector<std::uint32_t> starts;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t at = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(i) * phentsize;
        if (at + 32 > elf.size()) break;
        const std::uint32_t type = u32(at), vaddr = u32(at + 8), filesz = u32(at + 16), flags = u32(at + 24);
        if (type != 1 || filesz || !(flags & 1)) continue;  // no PT_LOAD, has contents, not executable
        if (std::find(starts.begin(), starts.end(), vaddr) != starts.end()) continue;
        starts.push_back(vaddr);
        std::uint32_t base = vaddr & ~(kWindowUnit - 1);
        while (std::find(out.begin(), out.end(), base) != out.end()) base += kWindowUnit;
        out.push_back(base);
    }
    return out;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> file_segments(const std::vector<std::uint8_t> &elf) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    if (elf.size() < 52) return out;
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 <= elf.size()) std::memcpy(&v, elf.data() + at, 4);
        return v;
    };
    const auto u16 = [&](std::size_t at) {
        std::uint16_t v = 0;
        if (at + 2 <= elf.size()) std::memcpy(&v, elf.data() + at, 2);
        return v;
    };
    const std::uint32_t phoff = u32(28);
    const std::uint16_t phentsize = u16(42), phnum = u16(44);
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t at = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(i) * phentsize;
        if (at + 32 > elf.size()) break;
        if (u32(at) == 1 && u32(at + 16)) out.emplace_back(u32(at + 8), u32(at + 20));
    }
    return out;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> code_segments(const std::vector<std::uint8_t> &elf) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    if (elf.size() < 52) return out;
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 <= elf.size()) std::memcpy(&v, elf.data() + at, 4);
        return v;
    };
    const auto u16 = [&](std::size_t at) {
        std::uint16_t v = 0;
        if (at + 2 <= elf.size()) std::memcpy(&v, elf.data() + at, 2);
        return v;
    };
    const std::uint32_t phoff = u32(28);
    const std::uint16_t phentsize = u16(42), phnum = u16(44);
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t at = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(i) * phentsize;
        if (at + 32 > elf.size()) break;
        const std::uint32_t type = u32(at), vaddr = u32(at + 8), filesz = u32(at + 16), memsz = u32(at + 20), flags = u32(at + 24);
        if (type == 1 && filesz && (flags & 1)) out.emplace_back(vaddr, memsz);
    }
    return out;
}

std::uint32_t window_extent(const std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t base) {
    // The ELF only says the window begins here; what the game unpacks decides how far it reaches, so the image covers the units it
    // wrote, stopping at the first entirely empty one.
    std::uint32_t size = 0;
    while (size < kWindowCap && base <= ram_size && size + kWindowUnit <= ram_size - base) {
        const std::uint8_t *unit = ram + base + size;
        bool empty = true;
        for (std::uint32_t i = 0; i < kWindowUnit; i += 8) {
            std::uint64_t word;
            std::memcpy(&word, unit + i, 8);
            if (word) { empty = false; break; }
        }
        if (empty) break;
        size += kWindowUnit;
    }
    return size ? size : kWindowUnit;
}

std::string window_name(std::uint32_t base, const std::uint8_t *image, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ image[i]) & 0xFFu] ^ (crc >> 8);
    crc ^= 0xFFFFFFFFu;
    char text[32];
    std::snprintf(text, sizeof text, "%08X_%08X", base, crc);
    return text;
}

// ---------------------------------------------------------------------------------------------------------------- live

StaticProgram::StaticProgram(const std::string &elf_path, const std::vector<std::uint32_t> &entries)
    : StaticProgram(Image::load(elf_path), entries) {}

StaticProgram::StaticProgram(Image image, const std::vector<std::uint32_t> &entries) : image_(std::move(image)) {
    const Links imports = snr2_imports(image_);
    program_ = analyse(image_, entries, dynamic_sites(imports));
}

std::vector<std::uint32_t> StaticProgram::calls_into(std::uint32_t base, std::uint32_t size) const {
    return window_entries(image_, program_, base, size);
}

std::pair<std::uint32_t, std::uint32_t> StaticProgram::code_range() const {
    std::uint32_t first = program_.end, last = program_.base;
    for (std::uint32_t pc = program_.base; pc < program_.end; pc += 4)
        if (program_.is_code(pc)) {
            first = std::min(first, pc);
            last = pc + 4;
        }
    if (image_.text().second > image_.text().first) {
        first = std::min(first, image_.text().first);
        last = std::max(last, image_.text().second);
    }
    return first < last ? std::pair<std::uint32_t, std::uint32_t>{first, last} : std::pair<std::uint32_t, std::uint32_t>{0, 0};
}

// An overlay with a Metrowerks header ("MWo3": magic, number, its own address, size, .bss size; Grand Theft Auto: San Andreas's
// .NM files) in a window image: the header is found in the first 16 KiB where its address word names the place it sits. Only the
// overlay is the image's code: the words before it belong to the program, and the ones past its size and .bss are left over from
// a bigger overlay loaded before (the program calls the other overlays at fixed addresses, so those calls would seed the analysis
// into the left-overs and make them check words that differ from run to run). The words before are zeroed (zero words are never
// checked) and the image ends with the overlay. Images without such a header are left as they are.
void mask_overlay_leftovers(std::uint32_t base, std::vector<std::uint8_t> &bytes) {
    const auto word = [&](std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + at, 4);
        return v;
    };
    for (std::size_t at = 0; at + 0x20 <= bytes.size() && at < kWindowUnit; at += 16) {
        if (word(at) != 0x336F574Du || word(at + 8) != base + at) continue;
        const std::uint64_t end = (at + static_cast<std::uint64_t>(word(at + 12)) + word(at + 16) + 3u) & ~std::uint64_t{3};
        if (end <= at + 0x20 || end > bytes.size()) return;
        std::fill(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at), std::uint8_t{0});
        bytes.resize(static_cast<std::size_t>(end));
        if (bytes.size() < 0x400u) bytes.resize(0x400u, 0);
        return;
    }
}

LiveResult generate_live(const LiveRequest &request, const std::filesystem::path &out_dir) {
    LiveResult result;
    try {
        std::vector<std::uint8_t> bytes = request.image;
        mask_overlay_leftovers(request.base, bytes);
        const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
        if (size < 0x400u || (size & 3u)) {
            result.error = "the window image is too small or not a whole number of words";
            return result;
        }
        // The same steps as eerecomp takes for an image of overlays/: analyse it from its entry points, check every word of it that is
        // code, translate it as a module that sits at a fixed base.
        // The text range is the whole window: with {0, size}, which is right for a module that sits at address 0, the search for function
        // starts stopped at the last place the entries reached, and a window's other functions were each found by a pause of their own.
        const bool code_only = request.text_end > request.text_begin;
        const Image image = Image::raw(request.base, bytes, code_only ? std::pair{request.text_begin, request.text_end} : std::pair{request.base, request.base + size});
        // The first translation of an image looks through all of it (and takes every place after a return as a function start, so that the
        // short functions that only pointer tables name are there too: each missed one used to cost a pause of its own). A later one for an
        // image that is already in place translates just what the missed entry reaches, which is a fraction of the compile time.
        Search search;
        search.whole_image = !request.focused;
        search.after_return = !request.focused;
        search.candidates = request.candidates;
        search.served.insert(request.served.begin(), request.served.end());
        const auto began = std::chrono::steady_clock::now();
        Program program = analyse(image, request.entries, {}, search);
        result.analyse_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        if (request.entries.empty()) {
            // No entry point given: a window translated before the game has called into it (runtime/src/ee_live.cpp, background
            // translation). What the search for function starts found is what there is.
            for (const std::uint32_t entry : program.entries)
                if (program.is_code(entry)) result.accepted.insert(entry);
        }
        for (const std::uint32_t entry : request.entries) {
            if (program.entries.count(entry) && program.is_code(entry)) result.accepted.insert(entry);
            else result.rejected.insert(entry);
        }
        if (result.accepted.empty()) {
            result.error = "none of the entry points is code in the captured image";
            return result;
        }
        Snr2Module o;
        o.bytes = bytes;
        o.size = size;
        o.base = request.base;
        o.name = request.name;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> checks;
        std::vector<std::uint32_t> soft;
        if (request.sparse_checks) {
            HeadChecks heads = entry_head_checks(o, program);
            checks = std::move(heads.hard);
            soft = std::move(heads.soft);
        } else {
            checks = checks_for_window(o, program);
        }
        const std::vector<Translation> translations{{&image, &program, nullptr, request.prefix, false, request.name, size, std::move(checks), true, request.base, std::move(soft)}};
        for (std::uint32_t pc = program.base; pc < program.end; pc += 4) result.code += program.is_code(pc);
        CodegenOptions options;
        options.output = ps2::os::utf8(out_dir);
        options.live_stamp = request.stamp;
        options.quiet = true;
        const auto coding = std::chrono::steady_clock::now();
        generate(translations, options);
        result.codegen_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - coding).count();
        for (const auto &entry : std::filesystem::directory_iterator(out_dir))
            if (entry.path().extension() == ".cpp" && entry.path().filename() != "table.cpp") ++result.units;
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}

}  // namespace recomp
