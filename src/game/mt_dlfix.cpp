// Display-list fix-ups applied to every graphics task just before the RSP gets it (same hook as the
// inspector in mt_dldump.cpp).
//
// Narrow 304x224 layout: the boot logos and the seg71 front-end (menus, player/character/court
// select) draw into the left 304 columns of the game's 384-wide framebuffer, so RT64 shows them left
// of centre. In any frame that uses that layout, everything belonging to it is moved right by 40 px
// ((384-304)/2) and the two side margins are cleared to black:
//   - scissors 0..304 (and 0..344 from the instruction patches) become 40..344;
//   - viewports centred at x=152 (trans.x 608) are re-centred at 192 (trans.x 768);
//   - smaller scissor boxes inside the layout (Continue-screen portraits) move by 40 px too;
//   - texture/fill rectangles drawn under the narrow scissor are shifted by 40 px;
//   - a small display list clearing both margins is spliced in after the frame's first clear.
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "ultramodern/config.hpp"
#include <utility>
#include "recomp.h"

extern "C" void rt64_set_horizontal_crop(float left, float right);   // lib/rt64 (Mario Tennis patch)

namespace {
    constexpr uint32_t q(uint32_t px) { return px * 4; }          // 10.2 fixed point
    constexpr uint32_t shift = q(40);
    constexpr uint32_t margin_dl = 0x7FFC00;                       // top of 8 MiB RDRAM, unused by the game

    // Rectangles already shifted, keyed by RDRAM address: static display lists are replayed every
    // frame and must not be shifted again. Dynamic buffers get fresh words, which differ from these.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> shifted;

    uint32_t rd(uint8_t* rdram, uint32_t phys) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF)));
    }
    void wr(uint8_t* rdram, uint32_t phys, uint32_t v) {
        MEM_W(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF))) = (int32_t)v;
    }
    uint16_t rd16(uint8_t* rdram, uint32_t phys) {
        return (uint16_t)MEM_HU(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF)));
    }
    void wr16(uint8_t* rdram, uint32_t phys, uint16_t v) {
        MEM_H(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF))) = (int16_t)v;
    }
    uint32_t add12(uint32_t word, int pos, uint32_t amount) {
        uint32_t v = (word >> pos) & 0xFFF;
        v = (v + amount > 0xFFF) ? 0xFFF : v + amount;
        return (word & ~(0xFFFu << pos)) | (v << pos);
    }

    // Walks a task's display list (following G_DL calls and segment changes) and calls fn(addr, w0, w1)
    // for every command. fn returns false to stop.
    template <typename Fn>
    void walk(uint8_t* rdram, uint32_t task, Fn&& fn) {
        uint32_t seg[16] = {};
        uint32_t stack[32]; int sp = 0;
        uint32_t pc = (uint32_t)MEM_W(0x30, (gpr)(int32_t)task) & 0x1FFFFFFF;
        auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x1FFFFFFF; };
        for (int n = 0; n < 400000; n++) {
            uint32_t w0 = rd(rdram, pc), w1 = rd(rdram, pc + 4);
            uint32_t cur = pc; pc += 8;
            uint8_t op = w0 >> 24;
            if (!fn(cur, w0, w1, resolve)) return;
            switch (op) {
                case 0xDB: if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x1FFFFFFF; break;
                case 0xDE:
                    if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = pc;
                    pc = resolve(w1);
                    break;
                case 0xDF: if (sp == 0) return; pc = stack[--sp]; break;
                case 0xE4: case 0xE5: pc += 16; break;                       // RDPHALF_1 / RDPHALF_2
                default: break;
            }
        }
    }

    bool is_narrow_scissor(uint32_t w0, uint32_t w1) {
        uint32_t ulx = (w0 >> 12) & 0xFFF, lrx = (w1 >> 12) & 0xFFF;
        return (ulx == 0 || ulx == shift) && (lrx == q(304) || lrx == q(344));
    }

    // A smaller box inside the narrow layout, e.g. the two character portraits on the Continue screen
    // (x 26..112 and 192..278, issue #16). It belongs to the layout and moves with it.
    bool is_inner_scissor(uint32_t w0, uint32_t w1) {
        uint32_t ulx = (w0 >> 12) & 0xFFF, lrx = (w1 >> 12) & 0xFFF;
        return ulx > 0 && lrx > ulx && lrx <= q(304);
    }
}

// Matrix tags for RT64 frame interpolation (issues #14, #11, #8).
//
// With Framerate Display/Manual RT64 draws extra frames between the game's own and has to work out
// which object in this frame is which object in the last one. Without help it guesses from the
// matrices, and with fast or many objects it guesses wrong: the ball drops to 30 Hz for a moment,
// limbs stretch across the screen at camera cuts. RT64 attaches a matrix-group ID to an object's
// transform when the vertices are loaded after a matrix change, and pairs equal IDs between frames
// in draw order. So the first vertex load after each modelview load or pop is tagged with the
// address of those vertices: the game's model data stays put, so each limb, the ball and so on keep
// their ID from frame to frame, and different objects can no longer be mixed up. Vertex data that
// is not at the same address two tasks running (rebuilt every frame in alternating buffers) gets
// G_EX_ID_AUTO, i.e. RT64's own matching as before.
// Tried and dropped: G_EX_ORDER_AUTO (pairs same-ID copies by position: the ball gets paired with
// its own trail) and IDs that include the matrix address (the ball and its trail rotate through a
// ring of five matrices). Both made the ball worse; see NOTES.md.
//
// The vertex command is replaced by a call to a 32-byte "slot" {matrix group, the original command,
// end} in RDRAM 0x700000..0x7EEFFF, which the game never uses (it runs in 4 MiB). Every patch is
// undone at the start of the next task (if the game has not rewritten that spot already), so the
// game's own lists only carry a patch for the one task RT64 is processing, and the slot area just
// alternates between two halves. Nothing is changed with Framerate Original.
namespace {
    constexpr uint32_t tag_base = 0x700000, tag_half = 0x77000, prelude = 0x7EF000;
    constexpr uint32_t slot_size = 32;
    constexpr uint32_t ex_matrix_group = 0x6400000C;                // RT64 extended opcode 0x64, G_EX_MATRIXGROUP_V1
    // gEXMatrixGroupDecomposed, no push, modelview: position, rotation, scale, skew and perspective
    // interpolated, vertices skipped, tiles interpolated, linear order (Zelda64Recomp's "Normal" preset).
    constexpr uint32_t ex_group_flags = (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11) | (1u << 15);
    constexpr uint32_t ex_id_auto = 0xFFFFFFFF;

    struct Patch { uint32_t at, w0, w1, orig0, orig1; };
    std::vector<Patch> patches;                                     // applied to the previous task
    std::unordered_set<uint32_t> seen_prev, seen_cur;               // vertex addresses tagged, last task / this task
    uint32_t half = 0;

    bool in_tag_area(uint32_t phys) { return phys >= tag_base && phys < prelude + 0x1000; }

    constexpr uint32_t obj_table = 0x0D8410, obj_size = 0x788, obj_count = 20;   // func_800CCBE0's object slots
    const bool owner_ids_off = std::getenv("MT_TAG_NO_OWNER") != nullptr;      // A/B switch: IDs without the slot

    void tag_matrices(uint8_t* rdram, uint32_t task) {
        // Undo last task's patches first, whatever the settings are now.
        for (const Patch& p : patches) {
            if (rd(rdram, p.at) == p.w0 && rd(rdram, p.at + 4) == p.w1) { wr(rdram, p.at, p.orig0); wr(rdram, p.at + 4, p.orig1); }
        }
        patches.clear();

        using RR = ultramodern::renderer::RefreshRate;
        static const bool tag_off = std::getenv("MT_TAG_OFF") != nullptr;   // TEMP A/B switch
        if (tag_off || ultramodern::renderer::get_graphics_config().rr_option == RR::Original) { seen_cur.clear(); return; }

        // Enable RT64's extended commands for this task: data_ptr -> {gEXEnable, branch to the list}.
        uint32_t start = (uint32_t)MEM_W(0x30, (gpr)(int32_t)task) & 0x1FFFFFFF;
        if (start == prelude) start = rd(rdram, prelude + 12) & 0x1FFFFFFF;
        wr(rdram, prelude + 0, 0xE0525464);                             // gEXEnable (hook 0xE0, magic "RTd")
        wr(rdram, prelude + 4, 0x10000064);
        wr(rdram, prelude + 8, 0xDE010000);                             // G_DL branch
        wr(rdram, prelude + 12, 0x80000000u | start);
        MEM_W(0x30, (gpr)(int32_t)task) = (int32_t)(0x80000000u | prelude);

        std::swap(seen_prev, seen_cur);
        seen_cur.clear();
        half ^= 1;
        uint32_t next = tag_base + half * tag_half;
        const uint32_t end = next + tag_half;
        bool need_tag = false;
        // Which object-table slot the current modelview matrix belongs to (-1: none). The game keeps each
        // object's matrices inside its slot of the object table (20 x 0x788 bytes at 0x0D8410), and slots
        // do not move during a match. Many objects draw the same vertex data (all shadow meshes are 0x0D3AC8,
        // drawn sorted by distance), so the slot is folded into the ID: otherwise RT64 pairs them by draw
        // order, and when that order changes as the camera moves, an in-between frame puts one object's
        // shadow halfway towards another object's.
        int obj = -1;
        unsigned dup_ids = 0;                                       // tagged loads that share an ID with an earlier one
        int obj_stack[16]; int obj_sp = 0;
        walk(rdram, task, [&](uint32_t cur, uint32_t w0, uint32_t w1, auto& resolve) {
            const uint8_t op = w0 >> 24;
            if (op == 0xDA) {                                           // G_MTX
                if ((w0 & 0x04) == 0) {                                 // modelview (bit 2 = projection)
                    need_tag = true;
                    const uint32_t m = resolve(w1);
                    const int s = (m >= obj_table && m < obj_table + obj_size * obj_count) ? int((m - obj_table) / obj_size) : -1;
                    if ((w0 & 0x01) == 0 && obj_sp < 16) obj_stack[obj_sp++] = obj;   // F3DEX2: param ^ G_MTX_PUSH
                    if ((w0 & 0x02) != 0 || s >= 0) obj = s;            // load, or a multiply by an object's matrix
                }
            } else if (op == 0xD8) {                                    // G_POPMTX
                need_tag = true;
                for (uint32_t k = std::max<uint32_t>(1, w1 / 64); k > 0 && obj_sp > 0; k--) obj = obj_stack[--obj_sp];
            } else if (op == 0x01 && need_tag && !in_tag_area(cur)) {  // G_VTX, first after a matrix change
                need_tag = false;
                if (next + slot_size > end) return true;                // out of room: leave it to RT64
                const uint32_t addr = resolve(w1);
                const uint32_t key = (obj >= 0 && !owner_ids_off) ? (addr ^ (uint32_t(obj + 1) << 24)) : addr;
                if (!seen_cur.insert(key).second) dup_ids++;
                const uint32_t id = seen_prev.count(key) ? key : ex_id_auto;
                const uint32_t slot = next; next += slot_size;
                const uint32_t words[8] = { ex_matrix_group, id, ex_group_flags, 0, w0, w1, 0xDF000000, 0 };
                for (uint32_t i = 0; i < 8; i++) wr(rdram, slot + 4 * i, words[i]);
                const uint32_t p0 = 0xDE000000, p1 = 0x80000000u | slot;   // G_DL (push) to the slot
                wr(rdram, cur, p0);
                wr(rdram, cur + 4, p1);
                patches.push_back({ cur, p0, p1, w0, w1 });
            }
            return true;
        });
        if (static const bool dbg = std::getenv("MT_TAG_DBG") != nullptr; dbg) {
            static unsigned n = 0;
            if ((++n % 60) == 0) std::fprintf(stderr, "[tags] %u tagged loads, %u sharing an ID\n", (unsigned)seen_cur.size() + dup_ids, dup_ids);
        }
    }
}

// 16-bit colour image read back as 8-bit texels (issue #12, the spotlight on Bowser in the intro).
//
// The game darkens everything outside the spotlight with a mask: it fills a 384-wide RGBA16 image
// (0x238040) with 0xD800, draws the light cone into it, then loads the same memory as a 768-wide I8
// texture and draws it over the screen with s stepping 2 texels per pixel, as black with alpha =
// texel. On hardware every pixel lands on an even byte, the high byte of a mask pixel (0xD8, 85%
// dark). RT64 draws the rectangle at the output resolution, so s runs through the whole 2-texel
// step inside each game pixel and half of it samples the odd byte (0x00, no darkening): the picture
// came out barely darkened, with one bright stripe per game pixel.
// Fix: in a mask like that, fill colours get their high byte copied into the low byte, so both
// bytes of every pixel hold the same intensity and any sample gives the hardware result. Patches
// are undone at the start of the next task, like the matrix tags. The cone itself (vertex colours
// 0x40,0,0 at the rim to black in the middle) keeps a low byte of 0; that leaves at most a 25%
// difference at the very rim, which measured as no visible stripe.
namespace {
    std::vector<Patch> mask_patches;

    void fix_byte_masks(uint8_t* rdram, uint32_t task) {
        for (const Patch& p : mask_patches) {
            if (rd(rdram, p.at) == p.w0 && rd(rdram, p.at + 4) == p.w1) { wr(rdram, p.at, p.orig0); wr(rdram, p.at + 4, p.orig1); }
        }
        mask_patches.clear();
        static const bool off = std::getenv("MT_MASK_OLD") != nullptr;     // A/B switch: old behaviour
        if (off) return;

        // Pass 1: 16-bit colour images that are later loaded as 8-bit texels at twice the width.
        std::unordered_map<uint32_t, uint32_t> cimg16;                      // address -> width, 16-bit colour images
        std::unordered_set<uint32_t> masks;
        walk(rdram, task, [&](uint32_t, uint32_t w0, uint32_t w1, auto& resolve) {
            const uint8_t op = w0 >> 24;
            const uint32_t width = (w0 & 0xFFF) + 1, siz = (w0 >> 19) & 3, addr = resolve(w1);
            if (op == 0xFF && siz == 2) cimg16[addr] = width;
            else if (op == 0xFD && siz == 1) {
                auto it = cimg16.find(addr);
                if (it != cimg16.end() && it->second * 2 == width) masks.insert(addr);
            }
            return true;
        });
        if (masks.empty()) return;

        // Pass 2: fill colours set while one of those is the colour image.
        bool in_mask = false;
        walk(rdram, task, [&](uint32_t cur, uint32_t w0, uint32_t w1, auto& resolve) {
            const uint8_t op = w0 >> 24;
            if (op == 0xFF) in_mask = (((w0 >> 19) & 3) == 2) && masks.count(resolve(w1)) != 0;
            else if (op == 0xF7 && in_mask) {
                const uint32_t n1 = (w1 & 0xFF00FF00u) | ((w1 >> 8) & 0x00FF00FFu);
                if (n1 != w1) {
                    wr(rdram, cur + 4, n1);
                    mask_patches.push_back({ cur, w0, n1, w0, w1 });
                }
            }
            return true;
        });
    }
}

void mt_prof_gfx_task();
void mt_widescreen_update();
extern "C" void mt_bonus_characters(uint8_t* rdram);
void mt_widescreen_flyover_check(uint8_t* rdram, bool narrow_frame);

extern "C" void mt_fixup_gfx_task(uint8_t* rdram, uint32_t task) {
    mt_prof_gfx_task();
    mt_widescreen_update();
    mt_bonus_characters(rdram);
    // Pass 1: does this frame use the narrow layout?
    bool narrow_frame = false;
    walk(rdram, task, [&](uint32_t, uint32_t w0, uint32_t w1, auto&) {
        if ((w0 >> 24) == 0xED && is_narrow_scissor(w0, w1)) { narrow_frame = true; return false; }
        return true;
    });
    // TEMP (issue #15): MT_LOG_2D=<file> logs the 2D drawing of narrow-layout frames when it changes.
    // MT_LOG_2D_ALL=<s>: log every frame from s seconds after the first task, for MT_LOG_2D_LEN seconds
    // (default 8). MT_LOG_2D_ADDR=<hex>: only frames that mention that address.
    static const double log_all_from = std::getenv("MT_LOG_2D_ALL") ? std::atof(std::getenv("MT_LOG_2D_ALL")) : -1.0;
    static const double log_len = std::getenv("MT_LOG_2D_LEN") ? std::atof(std::getenv("MT_LOG_2D_LEN")) : 8.0;
    static const char* log_addr = std::getenv("MT_LOG_2D_ADDR");
    static const auto log_t0 = std::chrono::steady_clock::now();
    const double log_t = std::chrono::duration<double>(std::chrono::steady_clock::now() - log_t0).count();
    const bool log_all = log_all_from >= 0 && log_t >= log_all_from && log_t < log_all_from + log_len;
    // MT_DUMP_MEM=<hexaddr>:<hexsize>:<file>: during the MT_LOG_2D_ALL window, write that RDRAM range to <file> (overwritten each task).
    // MT_TEX_SWAP=<from>:<to>: every SETTIMG of <from> is redirected to <to> for this task (to identify which
    // object a texture belongs to). Undone at the next task.
    {
        static std::vector<std::pair<uint32_t, uint32_t>> swapped;               // address, original w1
        for (auto& p : swapped) wr(rdram, p.first + 4, p.second);
        swapped.clear();
        if (static const char* ts = std::getenv("MT_TEX_SWAP"); ts) {
            unsigned from = 0, to = 0;
            if (std::sscanf(ts, "%x:%x", &from, &to) == 2) {
                walk(rdram, task, [&](uint32_t at, uint32_t w0, uint32_t w1, auto& resolve) {
                    if ((w0 >> 24) == 0xFD && resolve(w1) == from) { swapped.push_back({ at, w1 }); wr(rdram, at + 4, 0x80000000u | to); }
                    return true;
                });
            }
        }
    }
    // MT_WATCH_THREAD=<hex>,...: a host thread samples those RDRAM words every 0.5 ms and prints each change
    // (with ms since start), to see writes the game makes between task starts.
    if (static const char* wt = std::getenv("MT_WATCH_THREAD"); wt) {
        static bool started = false;
        if (!started) {
            started = true;
            std::vector<uint32_t> addrs;
            for (const char* p = wt; *p; ) { char* e; addrs.push_back((uint32_t)std::strtoul(p, &e, 16)); if (*e != ',') break; p = e + 1; }
            std::thread([rdram, addrs]() {
                std::vector<uint32_t> last(addrs.size(), 0xDEADBEEF);
                const auto t0 = std::chrono::steady_clock::now();
                for (int n = 0; n < 4000000; n++) {
                    for (size_t i = 0; i < addrs.size(); i++) {
                        const uint32_t v = rd(rdram, addrs[i]);
                        if (v != last[i]) {
                            last[i] = v;
                            std::fprintf(stderr, "[watcht] %9.2f ms %06X=%08X\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), addrs[i], v);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }).detach();
        }
        static const auto tt0 = std::chrono::steady_clock::now();
        static unsigned tasks = 0;
        if ((++tasks % 1) == 0 && log_all) std::fprintf(stderr, "[watcht] %9.2f ms task start\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tt0).count());
    }
    // MT_WATCH=<hex>,<hex>,...: during the MT_LOG_2D_ALL window, print those RDRAM words at every task start.
    if (static const char* wl = std::getenv("MT_WATCH"); wl && log_all) {
        std::string line = "[watch]";
        for (const char* p = wl; *p; ) {
            char* e; const uint32_t a = (uint32_t)std::strtoul(p, &e, 16);
            char b2[32]; std::snprintf(b2, sizeof b2, " %06X=%08X", a, rd(rdram, a)); line += b2;
            p = (*e == ',') ? e + 1 : e; if (e == p && *p != 0 && *e != ',') break;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    // MT_DUMP_MEM_CIMG=<hex>: only dump tasks that draw into that colour image.
    // The task after a matching one is dumped too, to "<file>.next".
    bool dump_ok = true;
    std::string dump_suffix;
    if (static const char* dc = std::getenv("MT_DUMP_MEM_CIMG"); dc && log_all) {
        static bool dump_next = false;
        const uint32_t want = (uint32_t)std::strtoul(dc, nullptr, 16);
        dump_ok = false;
        walk(rdram, task, [&](uint32_t, uint32_t w0, uint32_t w1, auto& resolve) {
            if ((w0 >> 24) == 0xFF && resolve(w1) == want) { dump_ok = true; return false; }
            return true;
        });
        if (!dump_ok && dump_next) { dump_ok = true; dump_suffix = ".next"; }
        dump_next = dump_ok && dump_suffix.empty();
    }
    if (static const char* dm = std::getenv("MT_DUMP_MEM"); dm && log_all && dump_ok) {
        unsigned a = 0, n = 0; char path[260] = {};
        if (std::sscanf(dm, "%x:%x:%259s", &a, &n, path) == 3) {
            std::strncat(path, dump_suffix.c_str(), sizeof path - std::strlen(path) - 1);
            if (FILE* df = std::fopen(path, "wb")) {
                for (unsigned i = 0; i < n; i += 4) { uint32_t w = rd(rdram, a + i); uint8_t be[4] = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w) }; std::fwrite(be, 1, 4, df); }
                std::fclose(df);
                // The task's display list start, for walking the dump offline.
                if (FILE* tf = std::fopen((std::string(path) + ".task").c_str(), "w")) {
                    std::fprintf(tf, "%08X\n", (uint32_t)MEM_W(0x30, (gpr)(int32_t)task) & 0x1FFFFFFF);
                    std::fclose(tf);
                }
            }
        }
    }
    if (static FILE* lf = std::getenv("MT_LOG_2D") ? std::fopen(std::getenv("MT_LOG_2D"), "w") : nullptr; lf && (narrow_frame || log_all)) {
        static std::string last;
        static long written = 0;
        std::string s; char b[256];
        walk(rdram, task, [&](uint32_t cur, uint32_t w0, uint32_t w1, auto& resolve) {
            const uint8_t op = w0 >> 24;
            b[0] = 0;
            if (op == 0xFF && log_all) std::snprintf(b, sizeof b, "CIMG fmt%u siz%u w%u @%06X\n", (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1, w1 & 0xFFFFFF);
            else if (op == 0xFF) std::snprintf(b, sizeof b, "CIMG w%u @%06X\n", (w0 & 0xFFF) + 1, w1 & 0xFFFFFF);
            else if (op == 0xED) std::snprintf(b, sizeof b, "  SCISSOR %.2f,%.2f-%.2f,%.2f\n", ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0, ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0);
            else if (op == 0xFD) std::snprintf(b, sizeof b, "  TIMG fmt%u siz%u w%u @%06X\n", (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1, resolve(w1));
            else if (op == 0xF4 || op == 0xF3) std::snprintf(b, sizeof b, "  %s t%u %.2f,%.2f-%.2f,%.2f\n", op == 0xF4 ? "LOADTILE" : "LOADBLOCK", (w1 >> 24) & 7, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0, ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0);
            else if (op == 0xF2) std::snprintf(b, sizeof b, "  TILESIZE t%u %.2f,%.2f-%.2f,%.2f\n", (w1 >> 24) & 7, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0, ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0);
            else if (op == 0xF5) std::snprintf(b, sizeof b, "  SETTILE t%u fmt%u siz%u line%u tmem%u cms%u cmt%u masks%u maskt%u\n", (w1 >> 24) & 7, (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 >> 9) & 0x1FF, w0 & 0x1FF, (w1 >> 8) & 3, (w1 >> 18) & 3, (w1 >> 4) & 0xF, (w1 >> 14) & 0xF);
            else if (op == 0xE2 || op == 0xE3) std::snprintf(b, sizeof b, "  OTHERMODE_%c %08X %08X\n", op == 0xE3 ? 'H' : 'L', w0, w1);
            else if (op == 0xE4 || op == 0xE5) {
                const uint32_t h1 = rd(rdram, cur + 12), h2 = rd(rdram, cur + 20);
                std::snprintf(b, sizeof b, "  TEXRECT%s t%u %.2f,%.2f-%.2f,%.2f st %.3f,%.3f d %.4f,%.4f\n", op == 0xE5 ? "FLIP" : "", (w1 >> 24) & 7,
                    ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0,
                    (int16_t)(h1 >> 16) / 32.0, (int16_t)(h1 & 0xFFFF) / 32.0, (int16_t)(h2 >> 16) / 1024.0, (int16_t)(h2 & 0xFFFF) / 1024.0);
            }
            else if (op == 0xF6) std::snprintf(b, sizeof b, "  FILLRECT %.2f,%.2f-%.2f,%.2f\n", ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0);
            else if (op == 0x05 || op == 0x06 || op == 0x07) std::snprintf(b, sizeof b, "  TRI\n");
            else if (log_all && (op == 0xF7 || op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB || op == 0xFC || op == 0xD9 || op == 0xD7 || op == 0xEE))
                std::snprintf(b, sizeof b, "  %s %08X %08X\n", op == 0xF7 ? "FILLCOLOR" : op == 0xF8 ? "FOGCOLOR" : op == 0xF9 ? "BLENDCOLOR" : op == 0xFA ? "PRIMCOLOR" : op == 0xFB ? "ENVCOLOR" : op == 0xFC ? "COMBINE" : op == 0xD9 ? "GEOMODE" : op == 0xD7 ? "TEXTURE" : "PRIMDEPTH", w0, w1);
            else if (log_all && op == 0x01) std::snprintf(b, sizeof b, "  VTX n%u @%06X\n", (w0 >> 12) & 0xFF, resolve(w1));
            else if (log_all && op == 0xDA) std::snprintf(b, sizeof b, "  MTX %02X @%06X\n", w0 & 0xFF, resolve(w1));
            else if (log_all && op == 0xDC && (w0 & 0xFF) == 8) {
                const uint32_t vp = resolve(w1);
                std::snprintf(b, sizeof b, "  VIEWPORT scale %d,%d,%d trans %d,%d,%d\n", (int16_t)rd16(rdram, vp), (int16_t)rd16(rdram, vp + 2), (int16_t)rd16(rdram, vp + 4),
                    (int16_t)rd16(rdram, vp + 8), (int16_t)rd16(rdram, vp + 10), (int16_t)rd16(rdram, vp + 12));
            }
            if (b[0] && !(op <= 0x07 && op != 0x01 && s.size() >= 6 && s.compare(s.size() - 6, 6, "  TRI\n") == 0)) s += b;
            return true;
        });
        const bool addr_ok = log_addr == nullptr || s.find(std::string("@") + log_addr) != std::string::npos;
        if (addr_ok && s != last && written < 60000000) { written += (long)s.size(); std::fprintf(lf, "==== frame\n%s", s.c_str()); std::fflush(lf); last = s; }
    }
    // In Stretch mode the presenter shows only the narrow layout's columns (40..344 of 384), so
    // menus fill the window too; other frames show the whole picture. No effect in other modes.
    rt64_set_horizontal_crop(narrow_frame ? 40.0f / 384.0f : 0.0f, narrow_frame ? 344.0f / 384.0f : 1.0f);
    mt_widescreen_flyover_check(rdram, narrow_frame);
    tag_matrices(rdram, task);
    fix_byte_masks(rdram, task);
    if (!narrow_frame) return;

    // Pass 2: move the layout and splice in the margin clear.
    //
    // Menu transitions (issue #9): the game captures the screen into a 304-wide buffer by loading
    // columns 0..303 of the 384-wide framebuffer and drawing them at x 0..303. Our layout is at
    // 40..343, so while the colour image is not 384 wide ("capture"), rectangles and the scissor
    // are left alone and the framebuffer columns being loaded are moved by 40 instead.
    bool narrow = false, spliced = false, after_first_fill = false, fb_set_after_fill = false;
    bool capture = false;
    uint32_t timg_width = 0;
    uint32_t scissor_addr = 0, scissor_w0 = 0, scissor_w1 = 0;      // last scissor we moved, original words
    walk(rdram, task, [&](uint32_t cur, uint32_t w0, uint32_t w1, auto& resolve) {
        uint8_t op = w0 >> 24;
        switch (op) {
            case 0xED:                                                      // SETSCISSOR
                narrow = is_narrow_scissor(w0, w1);
                scissor_addr = 0;
                if (!narrow && !capture) {
                    auto it = shifted.find(cur);
                    bool done = (it != shifted.end()) && it->second.first == w0 && it->second.second == w1;
                    if (done || is_inner_scissor(w0, w1)) {
                        narrow = true;                                      // its rectangles move too
                        if (!done) {
                            uint32_t n0 = add12(w0, 12, shift), n1 = add12(w1, 12, shift);
                            wr(rdram, cur, n0);
                            wr(rdram, cur + 4, n1);
                            shifted[cur] = { n0, n1 };
                        }
                        break;
                    }
                }
                if (narrow && !capture) {
                    uint32_t n0 = (w0 & ~(0xFFFu << 12)) | (shift << 12);
                    uint32_t n1 = (w1 & ~(0xFFFu << 12)) | (q(344) << 12);
                    // The game's own words, to restore if a capture follows (a scissor in a static
                    // list that was already moved reads back as 0..344, which is fine for a capture).
                    scissor_addr = cur;
                    scissor_w0 = (w0 & ~(0xFFFu << 12));
                    scissor_w1 = w1;
                    if (n0 != w0) wr(rdram, cur, n0);
                    if (n1 != w1) wr(rdram, cur + 4, n1);
                }
                break;
            case 0xFD:                                                      // SETTIMG
                timg_width = (w0 & 0xFFF) + 1;
                break;
            case 0xF4:                                                      // LOADTILE
                if (capture && timg_width == 384) {
                    auto it = shifted.find(cur);
                    bool done = (it != shifted.end()) && it->second.first == w0 && it->second.second == w1;
                    if (!done) {
                        uint32_t n0 = add12(w0, 12, shift), n1 = add12(w1, 12, shift);
                        wr(rdram, cur, n0);
                        wr(rdram, cur + 4, n1);
                        shifted[cur] = { n0, n1 };
                    }
                }
                break;
            case 0xDC:                                                      // MOVEMEM
                if ((w0 & 0xFF) == 8) {                                     // G_MV_VIEWPORT
                    uint32_t vp = resolve(w1);
                    if (rd16(rdram, vp + 8) == q(152)) wr16(rdram, vp + 8, q(192));   // trans.x
                }
                break;
            case 0xE4: case 0xE5: case 0xF6:                                // TEXRECT / FLIP / FILLRECT
                if (op == 0xF6 && !after_first_fill) after_first_fill = true;
                if (narrow && !capture) {
                    auto it = shifted.find(cur);
                    bool done = (it != shifted.end()) && it->second.first == w0 && it->second.second == w1;
                    if (!done) {
                        uint32_t n0 = add12(w0, 12, shift), n1 = add12(w1, 12, shift);
                        wr(rdram, cur, n0);
                        wr(rdram, cur + 4, n1);
                        shifted[cur] = { n0, n1 };
                    }
                }
                break;
            case 0xFF: {                                                    // SETCIMG
                const bool was_capture = capture;
                capture = ((w0 & 0xFFF) + 1) != 384;
                if (capture && !was_capture && scissor_addr != 0) {
                    // The scissor in effect was moved to 40..344; put the game's one back for the capture.
                    wr(rdram, scissor_addr, scissor_w0);
                    wr(rdram, scissor_addr + 4, scissor_w1);
                    scissor_addr = 0;
                }
                if (after_first_fill && !fb_set_after_fill) { fb_set_after_fill = true; return true; }
                break;
            }
            default:
                break;
        }

        // Right after the frame's first clear and the switch to the colour image, replace the next
        // command with a call to the margin-clearing list (which re-runs that command and returns).
        // The cycle type is still FILL from the clear and the scissor is still full-width there.
        if (fb_set_after_fill && !spliced && op == 0xDE && w1 == (0x80000000u | margin_dl)) {
            spliced = true;                                                 // already spliced (static list)
        }
        if (fb_set_after_fill && !spliced && op != 0xFF && op != 0xDE && op != 0xDF && op != 0xE4 && op != 0xE5) {
            spliced = true;
            const uint32_t m = margin_dl;
            const uint32_t list[][2] = {
                { 0xF7000000, 0x00010001 },                                 // SETFILLCOLOR black (RGBA5551 x2)
                { 0xF6000000 | (q(39) << 12) | q(223), 0 },                 // FILLRECT   0,0 -  39,223
                { 0xF6000000 | (q(383) << 12) | q(223), (q(344) << 12) },   // FILLRECT 344,0 - 383,223
                { w0, w1 },                                                 // the replaced command
                { 0xDF000000, 0 },                                          // ENDDL
            };
            for (uint32_t i = 0; i < 5; i++) { wr(rdram, m + 8 * i, list[i][0]); wr(rdram, m + 8 * i + 4, list[i][1]); }
            wr(rdram, cur, 0xDE000000);                                     // G_DL push
            wr(rdram, cur + 4, 0x80000000u | m);
        }
        return true;
    });
}

