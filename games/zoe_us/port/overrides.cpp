/*
 * Zone of the Enders's host implementations of guest functions.
 *
 * An override replaces the translated function at its address: the way a port
 * redirects code that drives hardware the host provides differently, and the
 * way the PC features hook in (a settings menu in the game's own menus,
 * widescreen, the mouse's targets). Register them here with
 * ps2host::Override(address, function, "why"); the original stays callable as
 * F_<ADDRESS>(c). See the engine's docs/PORTING.md and the ps2-port skill.
 *
 * Override as little as possible: the game's own code is the point.
 */

#include "ps2host/host.h"
#include "ps2host/iop_vm.h"
#include "ps2host/memory.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"

#include "pipeline_keys.inc"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

void F_00450C40(lifted::Context& c);
void F_00450F58(lifted::Context& c);
void F_004597F0(lifted::Context& c);
void F_00459908(lifted::Context& c);
void F_00282A20(lifted::Context& c);
void F_00420E88(lifted::Context& c);
void F_00420FA0(lifted::Context& c);
void F_00422FF0(lifted::Context& c);
void F_00423240(lifted::Context& c);
void F_004235A0(lifted::Context& c);
void F_00461ED0(lifted::Context& c);
void F_0045ABE0(lifted::Context& c);
void F_0045B668(lifted::Context& c);
void F_004663B0(lifted::Context& c);
void F_00466400(lifted::Context& c);
void F_00466450(lifted::Context& c);
void F_004664A0(lifted::Context& c);
void F_00466E38(lifted::Context& c);
void F_00466F18(lifted::Context& c);
void F_00466FB8(lifted::Context& c);

namespace {

// ---- ASPECT RATIO / FIELD OF VIEW (M7 step 5; porting guide 4, 4b) ----
// Game facts (addresses, offsets, what the functions do - no code) from ZOE-Recompiled b6a11d0
// src/zoe_overrides.cpp (GPL-3.0, as config/names.csv's names from its toml): a camera keeps a projection scale
// pair, +0x224 horizontal (1.0) and +0x228 vertical (0.58333 for the game's own camera), and its focal constant at
// +0x244 (500). 0x00450C40(camera) builds the camera's matrices +0x80 / +0xC0 from them (x scale +0x224 / 512, y
// +0x228 / 224) and keeps them; 0x00450F58(camera) composes them with the view each frame (its body skipped when
// the stamp +0x26C equals the game's frame counter 0x004AA288). A wider picture is the same 512 columns holding
// more of the world - the horizontal scale times (4/3) / aspect - which the window then shows at that aspect
// (the engine's anamorphic render: wide pictures get their columns back). The fields are only changed for the
// build and put back after (the game never sees them changed).
constexpr u32 kCamHScale = 0x224u, kCamVScale = 0x228u, kCamFocal = 0x244u, kCamStamp = 0x26Cu, kCamMatrix = 0x80u;
constexpr u32 kFrameCounter = 0x004AA288u;
constexpr u32 kBitsOne = 0x3F800000u, kBitsStockV = 0x3F155555u;     // the game's own camera: 1.0, 0.58333

float F32(u32 bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
u32 Bits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
u32 Read32(u32 address) { return u32(ps2host::GuestMemory().Read(address, 4)); }
void Write32(u32 address, u32 value) { ps2host::GuestMemory().Write(address, value, 4); }
bool CameraOk(u32 camera) { return camera >= 0x00100000u && camera < 0x02000000u - 0x300u; }

// The factors a camera's pair is built with: horizontal (4/3) / aspect for every camera (a cutscene's or a
// map window's as the game's own: the picture is wider for all of them); FIELD OF VIEW a zoom out of both, for the
// game's own camera (the stock pair) alone - +N degrees on its 4:3 horizontal view, 2 atan(256 / 500) = 54.2.
void Factors(u32 camera, float* h, float* v) {
    *h = (4.0f / 3.0f) / ps2host::WideAspect();
    *v = 1.0f;
    const int extra = ps2host::FieldOfViewExtra();
    if (extra > 0 && Read32(camera + kCamHScale) == kBitsOne && Read32(camera + kCamVScale) == kBitsStockV) {
        const float half = std::atan(256.0f / 500.0f);
        const float zoom = std::tan(half) / std::tan(half + float(extra) * 3.14159265f / 360.0f);
        *h *= zoom;
        *v = zoom;
    }
}

// 0x00450C40 with the camera's pair scaled for the build, put back after (if nothing else changed them since).
void BuildScaled(lifted::Context& c, u32 camera, float h, float v) {
    const u32 hold = Read32(camera + kCamHScale), vold = Read32(camera + kCamVScale);
    const u32 hnew = Bits(F32(hold) * h), vnew = Bits(F32(vold) * v);
    Write32(camera + kCamHScale, hnew);
    Write32(camera + kCamVScale, vnew);
    F_00450C40(c);
    if (Read32(camera + kCamHScale) == hnew) Write32(camera + kCamHScale, hold);
    if (Read32(camera + kCamVScale) == vnew) Write32(camera + kCamVScale, vold);
}

void CameraMatrix(lifted::Context& c) {
    const u32 camera = u32(c.r[4]);
    float h = 1.0f, v = 1.0f;
    if (CameraOk(camera)) Factors(camera, &h, &v);
    if (h == 1.0f && v == 1.0f) {
        F_00450C40(c);
        return;
    }
    BuildScaled(c, camera, h, v);
}

// Each frame a camera is used: its kept matrices built again where they are not this setting's (the setting
// changed, or the game set the camera up before) - told by +0x80's first element, -(+0x224) (+0x244) as built.
void CameraView(lifted::Context& c) {
    const u32 camera = u32(c.r[4]);
    if (CameraOk(camera) && Read32(camera + kCamStamp) != Read32(kFrameCounter)) {
        float h = 1.0f, v = 1.0f;
        Factors(camera, &h, &v);
        const float expected = -F32(Read32(camera + kCamHScale)) * h * F32(Read32(camera + kCamFocal));
        const float built = F32(Read32(camera + kCamMatrix));
        if (expected != 0.0f && std::fabs(built / expected - 1.0f) > 1e-4f) {
            // 0x00450C40(camera) first; the arguments and return address of 0x00450F58's own call kept.
            u64 saved[5], saved_hi[5];
            const int regs[5] = {4, 5, 6, 7, 31};
            for (int i = 0; i < 5; ++i) saved[i] = c.r[regs[i]], saved_hi[i] = c.rh[regs[i]];
            c.r[4] = camera;
            BuildScaled(c, camera, h, v);
            for (int i = 0; i < 5; ++i) c.r[regs[i]] = saved[i], c.rh[regs[i]] = saved_hi[i];
        }
    }
    F_00450F58(c);
}

// 3D drawn (0x004597F0, a 3D object's draw): the picture is the wide one while it was in the last few blanks;
// screens without it (title, menus on black, GAME OVER, movies) stand 4:3 between bars.
std::atomic<u64> g_last_3d_ns{0};
void Object3d(lifted::Context& c) {
    g_last_3d_ns.store(ps2host::LastVblankNs(), std::memory_order_relaxed);
    F_004597F0(c);
}
bool ShowsWide() {
    const u64 last = g_last_3d_ns.load(std::memory_order_relaxed);
    return last && ps2host::LastVblankNs() - last <= 3u * 16683334u;  // three blanks
}

// ---- HUD (M7 step 5, part 2): 0x00459908(object), a HUD sprite's draw - its packet built from the object
// (0x00459230): flags +0x02 (u16; 0x02 hidden, 0x20 GS coordinates, 0x8000 origin-centred), axis rows +0x20 /
// +0x30, position +0x50 / +0x54 (0..512 x 0..448), depth +0x58, vertices +0x8C (count), packet buffers +0x94 /
// +0x98 by byte +0x92 (ZOE-Recompiled b6a11d0, GPL-3.0: facts only).
// ZOE_HUD_LOG=<first blank>-<last blank> (diagnosis): each sprite drawn in those blanks, one line.
std::pair<u64, u64> HudLogRange();

// (0x00284870 builds a text-label options list - VIBRATION / CAPTION / SOUND from the table 0x0049B688 and Japanese
// debug lines - which the title never reaches: the title's OPTIONS screen is 0x00420E88's, its rows images.)

// Text objects (0x0045B668: subtitles, captions - its sprite part at +0x20, the sprite's layout 0x20 further on)
// and window containers (0x0045ABE0: panels with children) - logged for now (ZOE_HUD_LOG), placed next.
void HudLogOther(const char* what, u32 object, u32 at) {
    static const std::pair<u64, u64> range = [] {   // ZOE_HUD_LOG_OTHER=<first>-<last> (game frames)
        unsigned long long a = 1, b = 0;
        if (const char* e = std::getenv("ZOE_HUD_LOG_OTHER")) std::sscanf(e, "%llu-%llu", &a, &b);
        return std::make_pair(u64(a), u64(b));
    }();
    if (range.first > range.second || !CameraOk(object)) return;
    const u64 n = Read32(kFrameCounter);
    if (n < range.first || n > range.second) return;
    std::fprintf(stderr, "[hud] frame %llu %s %08X flags %04X pos %.1f,%.1f ax %.3f ay %.3f\n", (unsigned long long)n,
                 what, unsigned(object), unsigned(ps2host::GuestMemory().Read(object + at + 2u, 2)),
                 F32(Read32(object + at + 0x50u)), F32(Read32(object + at + 0x54u)), F32(Read32(object + at + 0x20u)),
                 F32(Read32(object + at + 0x34u)));
}
// Words of guest memory changed for one draw and put back after it (only if still what was written).
struct Changed {
    u32 at[24];
    u32 was[24];
    u32 now[24];
    int n = 0;
    void Put(u32 address, u32 value) {
        if (n == 24) return;
        at[n] = address;
        was[n] = Read32(address);
        now[n] = value;
        Write32(address, value);
        ++n;
    }
    void PutF(u32 address, float value) { Put(address, Bits(value)); }
    void Back() {
        for (int i = n - 1; i >= 0; --i)
            if (Read32(at[i]) == now[i]) Write32(at[i], was[i]);
        n = 0;
    }
};

float WideS() { return (4.0f / 3.0f) / ps2host::WideAspect(); }

// Where x lands for a piece anchored by its centre `centre` (the zones, as HudSprite): the map's offset b in
// x' = S x + b.
float ZoneOffset(float centre, float s) {
    if (centre < 215.0f) return 0.0f;
    if (centre >= 297.0f) return 512.0f * (1.0f - s);
    return 256.0f * (1.0f - s);
}

// A sprite object's vertex extent in x (its drawn packet buffer: vertices of 0x30 bytes, x at +0x20), false when
// it cannot be read.
bool SpriteExtent(u32 object, float* lo, float* hi) {
    const u32 count = Read32(object + 0x8Cu);
    const u32 buffer = Read32(object + (ps2host::GuestMemory().Read(object + 0x92u, 1) ? 0x94u : 0x98u));
    if (!count || count > 512u || !CameraOk(buffer)) return false;
    *lo = 1e9f;
    *hi = -1e9f;
    for (u32 v = 0; v < count; ++v) {
        const float x = F32(Read32(buffer + v * 0x30u + 0x20u));
        if (!(x > -4096.0f && x < 4096.0f)) return false;
        *lo = std::min(*lo, x);
        *hi = std::max(*hi, x);
    }
    return true;
}

// 0x0045B668(text object): a text standing alone (messages, subtitles; its sprite part at +0x20, the sprite's
// layout 0x20 on: flags +0x22, axes +0x40 / +0x50, place +0x70). Narrowed by S, kept at its edge - between the
// zones about its own place (centred text stays centred).
void HudText(lifted::Context& c) {
    const u32 object = u32(c.r[4]);
    HudLogOther("text", object, 0x20u);
    const float s = WideS();
    if (s >= 0.999f || !CameraOk(object) || !ShowsWide() || (ps2host::GuestMemory().Read(object + 0x22u, 2) & 0x22u)) {
        F_0045B668(c);
        return;
    }
    const float x = F32(Read32(object + 0x70u));
    float to = x;
    if (x < 215.0f) to = s * x;
    else if (x >= 297.0f) to = 512.0f - s * (512.0f - x);
    Changed changed;
    changed.PutF(object + 0x70u, to);
    changed.PutF(object + 0x40u, F32(Read32(object + 0x40u)) * s);
    changed.PutF(object + 0x50u, F32(Read32(object + 0x50u)) * s);
    F_0045B668(c);
    changed.Back();
}

// 0x0045ABE0(container): a window - its children (list from +0x8C, sentinel +0xA0, next at +0xC; type byte +0: 1
// sprite, 4 text, 2 callback) drawn through its matrix +0x20..+0x5F when flag 0x40 is set, else at their own
// coordinates. With every visible child inheriting the matrix (sprites with 0x4 and none of 0x400 / 0x80 / 0x20,
// texts whose part has 0x4 and not 0x22, the clip callbacks 0x002137A0 / 0x00213800), the whole window is moved by
// one map x' = S x + b on the matrix's x column (b by the clip rectangle's centre, the zones; none: the screen's
// centre) and the clip rectangle with it. A window without its own matrix whose matrix words are the identity gets
// the map as its matrix and the flag for the draw. Left alone: full-screen layers (a child or the clip 90 % of the
// width: the fade / scan-line layers), windows a child does not inherit (the map screen's 3D map window).
// (ZOE-Recompiled b6a11d0, GPL-3.0: facts only.)
constexpr u32 kClipFn = 0x002137A0u, kClipEndFn = 0x00213800u;
// ZOE_MENU_TRACE=1 (diagnosis): each menu label made (0x00282A20(menu, ?, text, ...)), its text and caller.
void MenuLabel(lifted::Context& c) {
    static const bool trace = std::getenv("ZOE_MENU_TRACE") != nullptr;
    if (trace) {
        std::string text;
        const u32 at = u32(c.r[6]);
        for (u32 i = 0; CameraOk(at) && i < 40; ++i) {
            const char ch = char(ps2host::GuestMemory().Read(at + i, 1));
            if (!ch) break;
            text += (ch >= 32 && ch < 127) ? ch : '?';
        }
        std::fprintf(stderr, "[menu] frame %u label a0 %08X a1 %08X text %08X '%s' ra %08X\n", unsigned(Read32(kFrameCounter)),
                     unsigned(c.r[4]), unsigned(c.r[5]), unsigned(at), text.c_str(), unsigned(c.r[31]));
    }
    F_00282A20(c);
}

// ---- The title's OPTIONS screen and RECOMP OPTIONS in it (M7 step 7, M9; the maintainer 2026-11-20: the player's
// settings in the game's own menu, the engine's panel only for debugging). The screen (object a0 of 0x00420E88, its
// state at +0x18000): rows 0 VIBRATION, 1 CAPTION DEMO, 2 CAPTION GAME, 3 SOUND, 4 DEFAULT, 5 EXIT at +0x189F4, a
// move's wait at +0x189EC; its rows are images (sprites through 0x00459908, rows at y 126 / 165 / 190 / 228 / 267 /
// 307 of 448, labels at x 82, values at 275 / 375), 0x00422FF0 moves the cursor (held pad 0x004AA4E0: 0x10 up, 0x40
// down - 6 rows wrapping; left / right change the row's value), 0x00420FA0 acts on a press (0x004AA4D6: 0x2000
// confirm, 0x4000 back), 0x00423240 / 0x004235A0 draw the green frame at the row's place from the table 0x004D1380
// (two sub-rows of four floats per row). The configuration word 0x004DFCA0: bit 0 vibration, 1 captions in the
// demos (the cutscenes - checked: off, the opening's lines are gone), 2 captions in the game, 3 stereo; set through
// the game's own setters (0x004663B0 / 0x00466400 / 0x00466450 / 0x004664A0, a0 on), which mark it for the save.
//
// The maintainer's order (2026-11-20): OPTIONS shows VIBRATION, SOUND, RECOMP OPTIONS, EXIT - CAPTION and DEFAULT
// hidden (their sprites not drawn, the cursor passing their rows), SOUND and EXIT moved up; RECOMP OPTIONS is a
// virtual row (the game on EXIT's row, our flag set). RECOMP OPTIONS opens our pages in the same screen: the game's
// rows hidden, ours drawn by the engine (SetGameTexts: Liberation Sans Bold in the game's colours, its letters
// ~1.2 wider), the green frame moved to our row. Pages: the groups, POST PROCESSING / DISPLAY / FRAME RATE / SHADERS
// / LANGUAGE (SUBTITLES CUTSCENES, SUBTITLES GAMEPLAY - the captions moved here -, VOICES with the Japanese disc,
// SUBTITLE LANGUAGE). Up / down choose, left / right change, circle confirms, cross goes back, triangle sets the
// line back to its default; leaving with a start-only line changed asks RESTART NEEDED.
constexpr u32 kOptionsState = 0x18000u, kOptionsRow = 0x9F4u, kOptionsWait = 0x9ECu;
constexpr u32 kPadHeld = 0x004AA4E0u, kPadPressed = 0x004AA4D6u, kFrameTable = 0x004D1380u;
constexpr u32 kConfigWord = 0x004DFCA0u, kLanguageWord = 0x004DFCA4u;
constexpr u32 kHeldUp = 0x10u, kHeldDown = 0x40u, kHeldLeft = 0x80u, kHeldRight = 0x20u;
constexpr u32 kPressConfirm = 0x2000u, kPressBack = 0x4000u, kPressDefault = 0x1000u;
constexpr int kExitRow = 5, kSoundRow = 3, kVibrationRow = 0;
constexpr float kGameRowY[6] = {126.0f, 165.0f, 190.0f, 228.0f, 267.0f, 307.0f};
constexpr float kSoundY = 165.0f, kRecompY = 204.0f, kExitY = 243.0f;    // OPTIONS' rows after the cleanup
constexpr float kPageTop = 120.0f, kPageStep = 33.0f;                    // our pages' rows
// The game's lettering, measured on its OPTIONS screen: capitals 8-9 of 448 lines, labels grey, the chosen value
// red, the other dark.
constexpr float kLetters = 8.5f, kStretch = 1.2f;
constexpr u32 kGrey = 0x727272u, kRed = 0x9E1F1Fu, kDark = 0x484848u, kTitle = 0x8A8A8Au;
constexpr u32 kWordGrey = kGrey;        // the game's rows' words as shown (white texels times 0x80, drawn in fields)
bool g_recomp_row = false;              // the cursor on RECOMP OPTIONS
u32 g_options_seen = 0;                 // the game frame the OPTIONS screen last ran in
u32 g_options_obj = 0;

bool PageOpen();                        // (our pages, below)
void PageClosed();

// A game sound (0x00461ED0(id, 0x3F, 0x20): 0x39 the cursor's move, 0x3A a confirm), the caller's registers kept.
void GameSound(lifted::Context& c, u32 id) {
    u64 saved[5], saved_hi[5];
    const int regs[5] = {4, 5, 6, 7, 31};
    for (int i = 0; i < 5; ++i) saved[i] = c.r[regs[i]], saved_hi[i] = c.rh[regs[i]];
    c.r[4] = id;
    c.r[5] = 0x3F;
    c.r[6] = 0x20;
    F_00461ED0(c);
    for (int i = 0; i < 5; ++i) c.r[regs[i]] = saved[i], c.rh[regs[i]] = saved_hi[i];
}

// One of the game's configuration setters (a0: on), the caller's registers kept.
void GameSetter(lifted::Context& c, void (*setter)(lifted::Context&), bool on) {
    u64 saved[5], saved_hi[5];
    const int regs[5] = {2, 3, 4, 5, 31};
    for (int i = 0; i < 5; ++i) saved[i] = c.r[regs[i]], saved_hi[i] = c.rh[regs[i]];
    c.r[4] = on ? 1 : 0;
    setter(c);
    for (int i = 0; i < 5; ++i) c.r[regs[i]] = saved[i], c.rh[regs[i]] = saved_hi[i];
}

u32 GameLanguage();

}  // namespace
bool ReadZoeDatFile(const std::string& path, std::vector<unsigned char>* out);     // dub.cpp
namespace {

// ---- Our pages. A line: a group (opens its page), an engine setting (PcSettings' index), a caption bit of the
// configuration word, EXIT, or RESTART NEEDED's two.
enum Page { kNoPage = -1, kTopPage, kPostPage, kDisplayPage, kRatePage, kShaderPage, kLanguagePage, kRestartPage };
enum LineKind { kGroup, kSetting, kCaption, kExitLine, kRestartNow, kRestartLater };
struct Line {
    LineKind kind;
    std::string label;
    int arg = 0;                        // the page, the setting's index, the caption's bit
    std::string value;                  // shown
    std::vector<std::string> values;    // two: shown side by side as the game's own (ON / OFF)
};
int g_page = kNoPage;
int g_page_row = 0;
int g_page_wait = 0;
bool g_frame_moved = false;             // the green frame to be built again (the screen's +0x9F8, as a row move sets)
u32 g_sound_drawn = 0;                  // the frame SOUND's word was last drawn in, its fade and its slide
float g_sound_alpha = 1.0f, g_sound_dx = 0.0f;
u32 g_stereo_drawn = 0;                 // and STEREO's (the red word passing over SOUND on the way in), its place
float g_stereo_x = 259.0f;
std::vector<std::pair<std::string, std::string>> g_restart_seen;   // start-only lines when the pages opened

const char* PageName(int page) {
    switch (page) {
        case kPostPage: return "POST PROCESSING";
        case kDisplayPage: return "DISPLAY";
        case kRatePage: return "FRAME RATE";
        case kShaderPage: return "SHADERS";
        case kLanguagePage: return "LANGUAGE";
        case kRestartPage: return "RESTART NEEDED";
        default: return "";
    }
}

// The engine's lines for a page (by its panel page or, for DISPLAY and SHADERS, by name).
int PageOfSetting(const ps2host::PcSetting& s) {
    if (s.page == "POST PROCESSING") return kPostPage;
    if (s.page == "FRAME RATE") return kRatePage;
    if (s.label == "DISPLAY MODE" || s.label == "RENDER RESOLUTION" || s.label == "ASPECT RATIO" ||
        s.label == "FIELD OF VIEW")
        return kDisplayPage;
    if (s.label == "SHADER COMPILE" || s.label == "SHADER NOTICE") return kShaderPage;
    if (s.label == "LANGUAGE" || s.label == "DUB") return kLanguagePage;
    return kTopPage;                    // TEXTURES (with a pack)
}

std::vector<Line> PageLines(int page) {
    std::vector<Line> lines;
    const std::vector<ps2host::PcSetting> settings = ps2host::PcSettings();
    auto setting = [&](int i) {
        const ps2host::PcSetting& s = settings[std::size_t(i)];
        std::string label = s.label;
        if (label == "DUB") label = "VOICES";
        if (label == "LANGUAGE") label = "SUBTITLE LANGUAGE";
        lines.push_back({kSetting, label, i, s.value, s.values.size() == 2 ? s.values : std::vector<std::string>{}});
    };
    if (page == kTopPage) {
        for (int group : {kPostPage, kDisplayPage, kRatePage, kShaderPage}) {
            bool any = false;
            for (const auto& s : settings) any = any || PageOfSetting(s) == group;
            if (any) lines.push_back({kGroup, PageName(group), group, "", {}});
        }
        for (int i = 0; i < int(settings.size()); ++i)
            if (PageOfSetting(settings[std::size_t(i)]) == kTopPage) setting(i);
        lines.push_back({kGroup, PageName(kLanguagePage), kLanguagePage, "", {}});
    } else if (page == kLanguagePage) {
        const u32 config = Read32(kConfigWord);
        lines.push_back({kCaption, "SUBTITLES CUTSCENES", 1, (config & 2u) ? "ON" : "OFF", {"ON", "OFF"}});
        lines.push_back({kCaption, "SUBTITLES GAMEPLAY", 2, (config & 4u) ? "ON" : "OFF", {"ON", "OFF"}});
        for (int i = 0; i < int(settings.size()); ++i)
            if (settings[std::size_t(i)].label == "DUB") setting(i);
        for (int i = 0; i < int(settings.size()); ++i)
            if (settings[std::size_t(i)].label == "LANGUAGE") setting(i);
    } else if (page == kRestartPage) {
        lines.push_back({kRestartNow, "RESTART NOW", 0, "", {}});
        lines.push_back({kRestartLater, "LATER", 0, "", {}});
        return lines;
    } else {
        for (int i = 0; i < int(settings.size()); ++i)
            if (PageOfSetting(settings[std::size_t(i)]) == page) setting(i);
    }
    lines.push_back({kExitLine, "EXIT", 0, "", {}});
    return lines;
}

bool PageOpen() { return g_page != kNoPage; }

// The start-only lines (VOICES: the disc is mapped at the start; SUBTITLE LANGUAGE is taken at once here).
std::vector<std::pair<std::string, std::string>> RestartLines() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& s : ps2host::PcSettings())
        if (s.restart && s.label != "LANGUAGE") out.push_back({s.label, s.value});
    return out;
}

void OpenPage(int page, int row = 0) {
    if (page == kTopPage && g_page == kNoPage) g_restart_seen = RestartLines();
    g_page = page;
    g_page_row = row;
    g_page_wait = 10;
    g_frame_moved = true;
}

void PageClosed() {
    if (g_page != kNoPage) g_frame_moved = true;
    g_page = kNoPage;
    g_page_row = 0;
}

// Back: a page to the groups (on its own row), the groups to OPTIONS - by RESTART NEEDED when a start-only line
// changed.
void PageBack() {
    if (g_page == kTopPage) {
        if (RestartLines() != g_restart_seen) {
            OpenPage(kRestartPage, 1);
            return;
        }
        PageClosed();
        return;
    }
    if (g_page == kRestartPage) {
        PageClosed();
        return;
    }
    const int from = g_page;
    const std::vector<Line> top = PageLines(kTopPage);
    int row = 0;
    for (int i = 0; i < int(top.size()); ++i)
        if (top[std::size_t(i)].kind == kGroup && top[std::size_t(i)].arg == from) row = i;
    OpenPage(kTopPage, row);
}

void ChangeLine(lifted::Context& c, const Line& line, int step) {
    if (line.kind == kSetting) {
        ps2host::StepPcSetting(line.arg, step);
        if (line.label == "SUBTITLE LANGUAGE") ps2host::GuestMemory().Write(kLanguageWord, GameLanguage(), 4);
    } else if (line.kind == kCaption) {
        GameSetter(c, line.arg == 1 ? F_00466400 : F_00466450, !(Read32(kConfigWord) & (1u << line.arg)));
    }
}

void DefaultLine(lifted::Context& c, const Line& line) {
    if (line.kind == kSetting) {
        ps2host::ResetPcSetting(line.arg);
        if (line.label == "SUBTITLE LANGUAGE") ps2host::GuestMemory().Write(kLanguageWord, GameLanguage(), 4);
    } else if (line.kind == kCaption) {
        GameSetter(c, line.arg == 1 ? F_00466400 : F_00466450, true);
    }
}

// The pages' input, each frame the OPTIONS screen runs (its own cursor and press calls skipped meanwhile).
void PageInput(lifted::Context& c) {
    const std::vector<Line> lines = PageLines(g_page);
    const int n = int(lines.size());
    if (g_page_row >= n) g_page_row = n - 1;
    const u32 held = u32(ps2host::GuestMemory().Read(kPadHeld, 2)) & 0xF0u;
    const u32 pressed = u32(ps2host::GuestMemory().Read(kPadPressed, 2)) & 0x7000u;
    if (g_page_wait > 0) --g_page_wait;
    const Line& line = lines[std::size_t(g_page_row)];
    if (g_page_wait == 0 && (held == kHeldUp || held == kHeldDown)) {
        g_page_row = (g_page_row + (held == kHeldDown ? 1 : n - 1)) % n;
        g_page_wait = 10;
        g_frame_moved = true;
        GameSound(c, 0x39);
    } else if (g_page_wait == 0 && (held == kHeldLeft || held == kHeldRight) &&
               (line.kind == kSetting || line.kind == kCaption)) {
        ChangeLine(c, line, held == kHeldRight ? 1 : -1);
        g_page_wait = 10;
        GameSound(c, 0x39);
    } else if (pressed == kPressConfirm && g_page_wait == 0) {
        GameSound(c, 0x3A);
        if (line.kind == kGroup) OpenPage(line.arg);
        else if (line.kind == kExitLine || line.kind == kRestartLater) PageBack();
        else if (line.kind == kRestartNow) ps2host::RequestRestart();
        else ChangeLine(c, line, 1);
        g_page_wait = 10;
    } else if (pressed == kPressBack) {
        GameSound(c, 0x3A);
        PageBack();
        g_page_wait = 10;
    } else if (pressed == kPressDefault && (line.kind == kSetting || line.kind == kCaption)) {
        DefaultLine(c, line);
        GameSound(c, 0x39);
    }
}

// RECOMP OPTIONS in the game's own menu lettering (the maintainer 2026-11-20: rebuilt from the menu font's own
// letters - these two words only, the pages keep Liberation Sans): its letters cut from the OPTIONS rows' word
// images in the player's own stage/title/title.pak (ZOE.DAT) - each word a 4-bit image (`stride` = its width
// rounded up to 8, 11 rows, a 48-byte head before it giving width and height at +8 / +10) with its 16-entry CLUT
// right after it (16-byte aligned; alpha 0..127 the coverage, colour white) - and set side by side as cut, at the
// columns where the game's own letters meet (the lowest coverage between them). Nothing of the game's is kept in
// this source: offsets and columns only.
struct Glyph {
    u32 word;                   // the word image's offset in title.pak
    u32 width;                  // its width (its head says so: checked)
    u32 from, to;               // the letter's columns
};
constexpr u32 kCaptionWord = 0x20260u, kDemoWord = 0x20680u, kExitWord = 0x20810u, kSoundWord = 0x20FE0u,
              kStereoWord = 0x211B0u, kVibrationWord = 0x213B0u;
constexpr u32 kWordRows = 11;

std::vector<std::uint8_t> WordMask(const std::vector<unsigned char>& pak, u32 at, u32 width) {
    if (at < 48 || at >= pak.size()) return {};
    const unsigned char* head = &pak[at - 48];
    if ((head[8] | head[9] << 8) != width || head[10] < kWordRows) return {};
    const u32 stride = (width + 7) / 8 * 8, bytes = (stride * head[10] / 2 + 15) / 16 * 16;
    if (at + bytes + 64 > pak.size()) return {};
    const unsigned char* clut = &pak[at + bytes];
    std::vector<std::uint8_t> mask(std::size_t(width) * kWordRows);
    for (u32 y = 0; y < kWordRows; ++y)
        for (u32 x = 0; x < width; ++x) {
            const u32 i = y * stride + x;
            const u32 index = (pak[at + i / 2] >> ((i & 1) * 4)) & 15u;
            mask[std::size_t(y) * width + x] = std::uint8_t(std::min(255u, u32(clut[index * 4 + 3]) * 2u));
        }
    return mask;
}

// The masks (11 rows each) of RECOMP OPTIONS and of EXIT (the game's own word, drawn by us so that both rows
// follow SOUND's animation), empty when the disc's file is not as expected (then the engine's letters / the game's
// EXIT).
struct WordMasks {
    std::vector<std::uint8_t> recomp, exit;
    u32 recomp_w = 0, exit_w = 0;
};
const WordMasks& OptionsMasks() {
    static WordMasks masks;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::vector<unsigned char> pak;
        bool ok = ReadZoeDatFile("stage/title/title.pak", &pak);
        std::vector<std::uint8_t>& built = masks.recomp;
        u32& built_width = masks.recomp_w;
        if (ok) {
            masks.exit = WordMask(pak, kExitWord, 32);
            masks.exit_w = masks.exit.empty() ? 0 : 32;
        }
        // R, E, T from STEREO; C, P from CAPTION; O, N, S from SOUND; M from DEMO; I from VIBRATION - all with their
        // ink on rows 1-10 (EXIT's T sits a row higher: not used).
        const Glyph r{kStereoWord, 56, 25, 35}, e{kStereoWord, 56, 17, 24}, cc{kCaptionWord, 60, 0, 10},
            o{kSoundWord, 48, 9, 19}, m{kDemoWord, 40, 17, 28}, p{kCaptionWord, 60, 19, 28}, t{kStereoWord, 56, 10, 17},
            i{kVibrationWord, 72, 9, 13}, n{kSoundWord, 48, 28, 38}, s{kSoundWord, 48, 0, 9};
        const std::vector<const Glyph*> letters = {&r, &e, &cc, &o, &m, &p, nullptr, &o, &p, &t, &i, &o, &n, &s};
        constexpr u32 kSpace = 4;
        std::vector<std::vector<std::uint8_t>> columns;     // the result's columns
        for (const Glyph* g : letters) {
            if (!ok) break;
            if (!g) {
                for (u32 k = 0; k < kSpace; ++k) columns.emplace_back(kWordRows, 0);
                continue;
            }
            const std::vector<std::uint8_t> word = WordMask(pak, g->word, g->width);
            if (word.empty()) {
                ok = false;
                break;
            }
            for (u32 x = g->from; x < g->to; ++x) {
                std::vector<std::uint8_t> column(kWordRows);
                for (u32 y = 0; y < kWordRows; ++y) column[y] = word[std::size_t(y) * g->width + x];
                columns.push_back(column);
            }
        }
        if (ok && !columns.empty()) {
            built_width = u32(columns.size());
            built.assign(std::size_t(built_width) * kWordRows, 0);
            for (u32 x = 0; x < built_width; ++x)
                for (u32 y = 0; y < kWordRows; ++y) built[std::size_t(y) * built_width + x] = columns[x][y];
        }
        std::fprintf(stderr, "[menu] RECOMP OPTIONS %s\n",
                     ok ? "in the game's own letters (title.pak)" : "in the engine's letters (title.pak not as expected)");
    }
    return masks;
}

}  // namespace
// Called when the disc is open (dub.cpp's hook, before the game starts): the words read then.
void WarmOptionsMenu() { OptionsMasks(); }
namespace {

// A mask at a row of the OPTIONS screen, placed as the game places its rows' words (a texel a column of 512, 1.07
// lines a row of 448, the first texel's centre half a texel in), in `colour`, faded by `alpha`.
ps2host::GameText MaskLabel(const std::vector<std::uint8_t>& mask, u32 width, float x, float y, u32 colour,
                            float alpha) {
    ps2host::GameText label;
    label.mask = mask;
    if (alpha < 1.0f)
        for (std::uint8_t& m : label.mask) m = std::uint8_t(m * alpha + 0.5f);
    label.mask_w = width;
    label.mask_h = kWordRows;
    label.x = x / 512.0f;
    label.y = (y - 0.5f) / 448.0f;
    label.width = float(width) / 512.0f;
    label.height = float(kWordRows) * 1.07f / 448.0f;
    label.colour = colour;
    return label;
}

// The texts of the OPTIONS screen this frame: RECOMP OPTIONS among the game's rows, or our page.
void OptionsTexts() {
    auto text = [](const std::string& s, float x, float y, u32 colour) {
        return ps2host::GameText{s, x / 512.0f, (y + 2.0f) / 448.0f, kLetters / 448.0f, colour, false, kStretch};
    };
    std::vector<ps2host::GameText> texts;
    if (!PageOpen()) {
        // RECOMP OPTIONS and EXIT with SOUND's animation (the maintainer 2026-11-20): shown while SOUND's word is
        // drawn, at its slide (x -80 -> 82, 16 a frame) and its fade; and as SOUND has its red STEREO passing over
        // it on the way in, a red copy of each word rides with STEREO until STEREO is in its place (x 259).
        const u32 now = Read32(kFrameCounter);
        const bool shown = g_sound_drawn && now - g_sound_drawn <= 1 && g_sound_alpha > 0.0f;
        if (!shown) {
            ps2host::SetGameTexts({});
            return;
        }
        const WordMasks& masks = OptionsMasks();
        // The game's words wait at x -80 (off the screen) until their row slides: a word wider than the one it
        // follows starts further left by the difference, so that it too waits unseen (RECOMP OPTIONS' end showed
        // at the left edge, waiting - the maintainer 2026-11-20), and gets to its place in the same frames.
        auto along = [](float x, float from, float to, u32 width, float followed) {
            const float p = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
            return x - std::max(0.0f, float(width) - followed) * (1.0f - p);
        };
        const float sound_x = 82.0f + g_sound_dx;
        const bool ghost = g_stereo_drawn && now - g_stereo_drawn <= 1 && g_stereo_x < 258.5f;
        if (!masks.recomp.empty()) {
            if (ghost)
                texts.push_back(MaskLabel(masks.recomp, masks.recomp_w,
                                          along(g_stereo_x, -80.0f, 259.0f, masks.recomp_w, 56.0f), kRecompY, kRed,
                                          g_sound_alpha));
            texts.push_back(MaskLabel(masks.recomp, masks.recomp_w, along(sound_x, -80.0f, 82.0f, masks.recomp_w, 48.0f),
                                      kRecompY, kWordGrey, g_sound_alpha));
        } else {
            texts.push_back(text("RECOMP OPTIONS", sound_x, kRecompY, kGrey));
        }
        if (!masks.exit.empty()) {
            if (ghost)
                texts.push_back(MaskLabel(masks.exit, masks.exit_w, along(g_stereo_x, -80.0f, 259.0f, masks.exit_w, 56.0f),
                                          kExitY, kRed, g_sound_alpha));
            texts.push_back(MaskLabel(masks.exit, masks.exit_w, along(sound_x, -80.0f, 82.0f, masks.exit_w, 48.0f),
                                      kExitY, kWordGrey, g_sound_alpha));
        }
        ps2host::SetGameTexts(texts);
        return;
    }
    texts.push_back(text(g_page == kTopPage ? std::string("RECOMP OPTIONS")
                                            : g_page == kRestartPage ? std::string(PageName(g_page))
                                                                     : std::string("RECOMP OPTIONS - ") + PageName(g_page),
                         82.0f, 66.0f, kTitle));
    const std::vector<Line> lines = PageLines(g_page);
    for (int i = 0; i < int(lines.size()); ++i) {
        const Line& l = lines[std::size_t(i)];
        const float y = kPageTop + kPageStep * float(i);
        texts.push_back(text(l.label, 82.0f, y, kGrey));
        if (l.values.size() == 2) {
            texts.push_back(text(l.values[0], 275.0f, y, l.value == l.values[0] ? kRed : kDark));
            texts.push_back(text(l.values[1], 375.0f, y, l.value == l.values[1] ? kRed : kDark));
        } else if (!l.value.empty()) {
            texts.push_back(text(l.value, 275.0f, y, kRed));
        }
    }
    ps2host::SetGameTexts(texts);
}

// A sprite of the OPTIONS screen's rows: hidden (true), or moved down by *dy (its y changed for the draw).
bool OptionsSprite(u32 object, float* dy) {
    *dy = 0;
    if (!g_options_seen || Read32(kFrameCounter) - g_options_seen > 1 || !CameraOk(object)) return false;
    // The rows' words slide in from x -80 (16 a frame) - matched by their row (y) and texture width alone, wherever
    // they are; a whole-width sprite (a line, a layer) is not a row's word.
    const float x = F32(Read32(object + 0x50u)), y = F32(Read32(object + 0x54u));
    if (y < 118.0f || y > 320.0f) return false;
    const u32 buffer = Read32(object + (ps2host::GuestMemory().Read(object + 0x92u, 1) ? 0x94u : 0x98u));
    const float w = CameraOk(buffer) ? F32(Read32(buffer + 0x50u)) : 0.0f;
    if (!(w > 0.0f && w < 200.0f)) return false;
    auto near = [&](float at) { return std::fabs(y - at) < 4.0f; };
    if (near(kGameRowY[kSoundRow])) {
        // SOUND's word (48 wide) and STEREO (56): RECOMP OPTIONS and EXIT follow them (OptionsTexts) - the
        // packet's vertex alpha at +0x1C (128 -> 0 over 10 frames on leaving), the place's x.
        const u32 now = Read32(kFrameCounter);
        if (std::fabs(w - 48.0f) < 0.5f) {
            g_sound_drawn = now;
            g_sound_alpha = std::clamp(F32(Read32(buffer + 0x1Cu)) / 128.0f, 0.0f, 1.0f);
            g_sound_dx = x - 82.0f;
        } else if (std::fabs(w - 56.0f) < 0.5f) {
            g_stereo_drawn = now;
            g_stereo_x = x;
        }
    }
    if (PageOpen()) return true;                                    // our page: the game's rows away
    if (near(kGameRowY[1]) || near(kGameRowY[2]) || near(kGameRowY[4])) return true;     // CAPTION, DEFAULT
    if (near(kGameRowY[kExitRow])) {
        if (!OptionsMasks().exit.empty()) return true;              // ours, with SOUND's animation
        *dy = kExitY - kGameRowY[kExitRow];
    }
    if (near(kGameRowY[kSoundRow])) *dy = kSoundY - kGameRowY[kSoundRow];
    return false;
}

void OptionsFrame(lifted::Context& c) {
    const u32 obj = u32(c.r[4]);
    const u32 frame = Read32(kFrameCounter);
    if (obj != g_options_obj || frame - g_options_seen > 4) {        // entered anew
        g_recomp_row = false;
        PageClosed();
    }
    g_options_obj = obj;
    g_options_seen = frame;
    F_00420E88(c);
    OptionsTexts();
}

// Off the OPTIONS screen: the texts gone (called where the game draws, each frame).
void OptionsLabelTick() {
    if (g_options_seen && Read32(kFrameCounter) - g_options_seen > 2) {
        ps2host::SetGameTexts({});
        g_options_seen = 0;
        g_recomp_row = false;
        PageClosed();
    }
}

// The cursor over the four rows left (VIBRATION, SOUND, RECOMP OPTIONS, EXIT): the game moves it over its six,
// past CAPTION's and DEFAULT's (moved on again at once); RECOMP OPTIONS between SOUND and EXIT is our flag on EXIT.
void OptionsCursor(lifted::Context& c) {
    const u32 state = u32(c.r[4]) + kOptionsState;
    // The game builds its green frame again only when its row changes: on our pages its row goes between DEFAULT's
    // and EXIT's (the same plain frame) at each move - the place is ours anyway (OptionsFrameDraw) -, back on EXIT's
    // when the pages close.
    if (PageOpen()) {                               // our page: its input, the game's cursor still
        PageInput(c);
        if (g_frame_moved) {
            Write32(state + kOptionsRow, Read32(state + kOptionsRow) == 4u ? kExitRow : 4u);
            Write32(state + kOptionsWait, 10);      // the frame moves while the move's wait runs down
        }
        g_frame_moved = false;
        if (Read32(state + kOptionsWait)) F_00422FF0(c);    // (only its wait counted down: no row move meanwhile)
        c.r[2] = 0;
        return;
    }
    if (g_frame_moved) {                            // back from a page: on RECOMP OPTIONS (EXIT's row, our flag)
        Write32(state + kOptionsRow, kExitRow);
        Write32(state + kOptionsWait, 10);
        g_frame_moved = false;
    }
    const u32 wait = Read32(state + kOptionsWait);
    const u32 held = u32(ps2host::GuestMemory().Read(kPadHeld, 2)) & 0xF0u;
    const u32 row = Read32(state + kOptionsRow);
    if (wait == 0 && row == u32(kExitRow) && ((g_recomp_row && held == kHeldDown) || (!g_recomp_row && held == kHeldUp))) {
        g_recomp_row = !g_recomp_row;               // RECOMP OPTIONS <-> EXIT: the game stays on EXIT's row
        Write32(state + kOptionsWait, 10);
        Write32(state + 0x9F8u, 1);                 // its frame built again
        GameSound(c, 0x39);
        c.r[2] = 0;
        return;
    }
    if (g_recomp_row && wait == 0 && held == kHeldUp) g_recomp_row = false;     // up from it: the game's move
    if (g_recomp_row) {                             // left / right: nothing on our line; the wait counted down
        if (held == kHeldLeft || held == kHeldRight) {
            c.r[2] = 0;
            return;
        }
        F_00422FF0(c);
        return;
    }
    F_00422FF0(c);
    // A hidden row reached (the game moves one row a press): on to the next shown one - down from VIBRATION SOUND,
    // down from SOUND RECOMP OPTIONS, up from SOUND VIBRATION. (Its move changes the row, the wait and plays the
    // sound only; the value column +0x9F0 is the screen's one toggle, not the row's.)
    const u32 now = Read32(state + kOptionsRow);
    if (now != row && (now == 1 || now == 2 || now == 4)) {
        if (now == 4 && held == kHeldDown) {
            Write32(state + kOptionsRow, kExitRow);
            g_recomp_row = true;
        } else if (now == 4) {
            Write32(state + kOptionsRow, kSoundRow);
        } else {
            Write32(state + kOptionsRow, held == kHeldDown ? kSoundRow : kVibrationRow);
        }
    }
}

void OptionsPress(lifted::Context& c) {
    if (PageOpen()) {
        c.r[2] = 0;
        return;
    }
    const u32 state = u32(c.r[4]) + kOptionsState;
    const u32 pressed = u32(ps2host::GuestMemory().Read(kPadPressed, 2)) & 0x7000u;
    const u32 row = Read32(state + kOptionsRow);
    if (pressed == kPressDefault && !g_recomp_row && (row == u32(kVibrationRow) || row == u32(kSoundRow))) {
        // Triangle: the row back to its default (vibration on, stereo), its value's column with it.
        GameSetter(c, row == u32(kVibrationRow) ? F_004663B0 : F_004664A0, true);
        Write32(state + 0x9F0u, 0);
        GameSound(c, 0x39);
        c.r[2] = 0;
        return;
    }
    if (!g_recomp_row) {
        F_00420FA0(c);
        return;
    }
    if (pressed == kPressConfirm && Read32(state + kOptionsWait) == 0) {    // confirm: our pages
        GameSound(c, 0x3A);
        OpenPage(kTopPage);
        c.r[2] = 0;
        return;
    }
    if (pressed == kPressBack) {                                    // back: as on any row
        g_recomp_row = false;
        F_00420FA0(c);
        return;
    }
    c.r[2] = 0;
}

// The green frame at the row shown: the drawn row's two sub-rows (four floats each) moved for the draw.
void OptionsFrameDraw(lifted::Context& c, void (*draw)(lifted::Context&)) {
    if (!g_options_obj || !CameraOk(g_options_obj)) {
        draw(c);
        return;
    }
    const u32 row = std::min<u32>(Read32(g_options_obj + kOptionsState + kOptionsRow), 5u);
    float to = kGameRowY[row];
    if (PageOpen()) to = kPageTop + kPageStep * float(g_page_row);
    else if (g_recomp_row) to = kRecompY;
    else if (row == u32(kSoundRow)) to = kSoundY;
    else if (row == u32(kExitRow)) to = kExitY;
    const float dy = to - kGameRowY[row];
    if (dy == 0.0f) {
        draw(c);
        return;
    }
    Changed changed;
    for (u32 i = 0; i < 2 * 4; ++i) {
        const u32 at = kFrameTable + (2 * row) * 16 + i * 4;
        changed.PutF(at, F32(Read32(at)) + dy);
    }
    draw(c);
    changed.Back();
}
void OptionsFrameA(lifted::Context& c) { OptionsFrameDraw(c, F_00423240); }
void OptionsFrameB(lifted::Context& c) { OptionsFrameDraw(c, F_004235A0); }

void HudWindow(lifted::Context& c) {
    OptionsLabelTick();
    const u32 window = u32(c.r[4]);
    HudLogOther("window", window, 0u);
    const float s = WideS();
    if (s >= 0.999f || !CameraOk(window) || !ShowsWide()) {
        F_0045ABE0(c);
        return;
    }
    const u16 flags = u16(ps2host::GuestMemory().Read(window + 2u, 2));
    bool own = (flags & 0x40u) != 0;
    bool identity = false;
    if (!own && ps2host::GuestMemory().Read(window, 1) == 3u) {
        identity = true;
        for (u32 i = 0; i < 16 && identity; ++i)
            identity = std::fabs(F32(Read32(window + 0x20u + i * 4u)) - ((i & 3u) == (i >> 2) ? 1.0f : 0.0f)) < 1e-4f;
    }
    if (!own && !identity) {
        F_0045ABE0(c);
        return;
    }
    // Its children: all inheriting? the clip rectangle, the widest sprite.
    u32 clip = 0;
    float widest = 0.0f;
    bool inherits = true;
    const u32 sentinel = window + 0xA0u;
    u32 child = Read32(window + 0x8Cu);
    for (u32 guard = 0; child != sentinel && inherits; ++guard, child = Read32(child + 0xCu)) {
        if (guard > 256u || !CameraOk(child)) {
            inherits = false;
            break;
        }
        const u16 f = u16(ps2host::GuestMemory().Read(child + 2u, 2));
        if (f & 2u) continue;                       // hidden: not drawn
        const u32 type = u32(ps2host::GuestMemory().Read(child, 1));
        if (type == 1u) {
            inherits = (f & 0x4u) && !(f & (0x400u | 0x80u | 0x20u));
            float lo = 0, hi = 0;
            if (SpriteExtent(child, &lo, &hi)) widest = std::max(widest, (hi - lo) * std::fabs(F32(Read32(child + 0x20u))));
            else if (identity) widest = 1e9f;       // a child that cannot be measured: as full-screen
        } else if (type == 4u) {
            const u16 sf = u16(ps2host::GuestMemory().Read(child + 0x22u, 2));
            inherits = (sf & 0x4u) && !(sf & 0x22u);
        } else if (type == 2u) {
            const u32 fn = Read32(child + 0xA0u);
            if (fn == kClipFn && CameraOk(Read32(child + 0xA4u))) clip = Read32(child + 0xA4u);
            else inherits = fn == kClipEndFn;
        } else {
            inherits = false;
        }
    }
    const float full = 0.9f * 512.0f;
    const bool full_screen = widest * std::fabs(F32(Read32(window + 0x20u))) >= full ||
                             (clip && F32(Read32(clip + 8u)) >= full);
    const float px = F32(Read32(window + 0x50u));
    if (!inherits || full_screen || (identity && clip) || !(px > -512.0f && px < 1024.0f)) {
        F_0045ABE0(c);
        return;
    }
    float b = 256.0f * (1.0f - s);
    if (clip) {
        const float rx = F32(Read32(clip)), rw = F32(Read32(clip + 8u));
        if (rw > 0.0f && rx > -512.0f && rx < 1024.0f) b = ZoneOffset(rx + 0.5f * rw, s);
        if (b != 0.0f && b != 512.0f * (1.0f - s)) b = (1.0f - s) * (rx + 0.5f * rw);   // between: about its centre
    }
    Changed changed;
    for (u32 r = 0; r < 4; ++r) {               // row r: x at +0, w at +0xC
        const u32 a = window + 0x20u + r * 0x10u;
        changed.PutF(a, s * F32(Read32(a)) + b * F32(Read32(a + 0xCu)));
    }
    if (clip) {
        changed.PutF(clip, s * F32(Read32(clip)) + b);
        changed.PutF(clip + 8u, s * F32(Read32(clip + 8u)));
    }
    if (identity) changed.Put(window, Read32(window) | 0x00400000u);    // flag 0x40 (+2's bit 6): its own matrix
    F_0045ABE0(c);
    changed.Back();
}

std::pair<u64, u64> HudLogRange() {
    static const std::pair<u64, u64> range = [] {
        unsigned long long a = 1, b = 0;
        if (const char* e = std::getenv("ZOE_HUD_LOG")) std::sscanf(e, "%llu-%llu", &a, &b);
        return std::make_pair(u64(a), u64(b));
    }();
    return range;
}
void HudSpriteDraw(lifted::Context& c);

// The OPTIONS screen's rows first (hidden or moved: OptionsSprite), then the sprite as below.
void HudSprite(lifted::Context& c) {
    OptionsLabelTick();
    float dy = 0;
    const u32 object = u32(c.r[4]);
    if (OptionsSprite(object, &dy)) return;                 // not drawn
    if (dy == 0.0f) {
        HudSpriteDraw(c);
        return;
    }
    Changed changed;
    changed.PutF(object + 0x54u, F32(Read32(object + 0x54u)) + dy);
    HudSpriteDraw(c);
    changed.Back();
}

void HudSpriteDraw(lifted::Context& c) {
    const u32 object = u32(c.r[4]);
    const auto range = HudLogRange();
    if (range.first <= range.second && CameraOk(object)) {
        const u64 n = Read32(kFrameCounter);
        // ZOE_HUD_LOG_EVERY=<k>: only every k-th game frame of the range (a played session's whole length).
        static const u64 every = [] {
            const char* e = std::getenv("ZOE_HUD_LOG_EVERY");
            return e ? std::max<u64>(1, std::strtoull(e, nullptr, 10)) : 1;
        }();
        static u64 said = ~0ull;    // the game's frame number beside the host's blank time, now and then
        if (n / 300 != said) {
            said = n / 300;
            std::fprintf(stderr, "[hud] game frame %llu at %.2f s\n", (unsigned long long)n,
                         double(ps2host::LastVblankNs()) / 1e9);
        }
        if (n >= range.first && n <= range.second && n % every == 0) {
            const u32 buffer = Read32(object + (ps2host::GuestMemory().Read(object + 0x92u, 1) ? 0x94u : 0x98u));
            std::fprintf(stderr,
                         "[hud] frame %llu obj %08X flags %04X pos %.1f,%.1f z %g ax %.3f,%.3f ay %.3f,%.3f verts %u "
                         "buf %08X w %g h %g\n",
                         (unsigned long long)n, unsigned(object), unsigned(ps2host::GuestMemory().Read(object + 2u, 2)),
                         F32(Read32(object + 0x50u)), F32(Read32(object + 0x54u)), F32(Read32(object + 0x58u)),
                         F32(Read32(object + 0x20u)), F32(Read32(object + 0x24u)), F32(Read32(object + 0x30u)),
                         F32(Read32(object + 0x34u)), unsigned(Read32(object + 0x8Cu)), unsigned(buffer),
                         CameraOk(buffer) ? F32(Read32(buffer + 0x50u)) : 0.0f,
                         CameraOk(buffer) ? F32(Read32(buffer + 0xC4u)) : 0.0f);
        }
    }
    // The wide picture's HUD: the same 512 columns hold a wider view, so each piece is narrowed by S = (4/3) /
    // aspect and kept at its edge - left of x 215 (its centre) by the left edge, from 297 by the right, between
    // by the screen's centre (ZOE-Recompiled's zones; all three the same S, only their anchor differs, so a row
    // of digits in one zone stays together). Pieces placed in GS coordinates (flag 0x20: a target box, from the
    // 3D view - already where the wide camera puts it) only narrowed about their place; a piece as wide as the
    // screen (an overlay, a fade) as it is: in the squeezed columns it covers the wide picture. The object's
    // place (+0x50) and its axes' x (+0x20, +0x30) changed for the packet's build alone, put back after.
    const float s = (4.0f / 3.0f) / ps2host::WideAspect();
    const u16 flags = u16(ps2host::GuestMemory().Read(object + 2u, 2));
    if (s >= 0.999f || !CameraOk(object) || (flags & 0x02u) || !ShowsWide()) {
        F_00459908(c);
        return;
    }
    const u32 buffer = Read32(object + (ps2host::GuestMemory().Read(object + 0x92u, 1) ? 0x94u : 0x98u));
    const float x = F32(Read32(object + 0x50u));
    float w = CameraOk(buffer) ? F32(Read32(buffer + 0x50u)) : 0.0f;
    if (!(w >= 0.0f && w <= 512.0f)) w = 0.0f;
    float to = x;
    if (flags & 0x20u) {
        to = x;                                     // GS coordinates: the wide camera placed it
    } else if (w >= 0.9f * 512.0f || (x == 256.0f && w >= 256.0f)) {
        F_00459908(c);                              // the whole width: as it is
        return;
    } else {
        // A piece starting at the left edge (x < 128) stays with it, one reaching the right edge (> 384) with that
        // - a bar's frame, its fill (whose length changes: by its centre it would jump zones as it shrinks) and
        // its name stay together (the boss bar: TEMPEST at 24, frame 86..494, fill from 88); others by their
        // centre's zone. Never stretched: a bar across the screen ends earlier in a wider picture.
        const float left = (flags & 0x8000u) ? x - w * 0.5f : x, right = left + w, centre = left + w * 0.5f;
        if (left < 128.0f || (right <= 384.0f && centre < 215.0f)) to = s * x;
        else if (right > 384.0f || centre >= 297.0f) to = 512.0f - s * (512.0f - x);
        else to = 256.0f + s * (x - 256.0f);
    }
    const u32 saved[3] = {Read32(object + 0x50u), Read32(object + 0x20u), Read32(object + 0x30u)};
    const u32 wrote[3] = {Bits(to), Bits(F32(saved[1]) * s), Bits(F32(saved[2]) * s)};
    Write32(object + 0x50u, wrote[0]);
    Write32(object + 0x20u, wrote[1]);
    Write32(object + 0x30u, wrote[2]);
    F_00459908(c);
    const u32 at[3] = {0x50u, 0x20u, 0x30u};
    for (int i = 0; i < 3; ++i)
        if (Read32(object + at[i]) == wrote[i]) Write32(object + at[i], saved[i]);
}

// The game's language index (0x004DFCA4, beside its flags at 0x004DFCA0 - the 12 bytes a save's configuration
// copies back, 0x00466E38): 1 English, 2 French, 3 German, 4 Italian, as the PAL version maps the console's
// language into it (SLES 0x00466C98, 2026-11-13). The US program sets it to English at start, asking nothing
// (0x00466388) - yet the US disc's voiced scenes carry all four languages' subtitles (docs/languages_research.md).
// The "language" setting (en / fr / de / it) chooses; unset, English as before.
u32 GameLanguage() {
    const std::string value = ps2host::Setting("language");
    if (value == "fr") return 2;
    if (value == "de") return 3;
    if (value == "it") return 4;
    return 1;
}

// 0x00466388 as it is - flags 0xF, the language, then 0x00466F18(0xFF000006) - with the language chosen.
void LanguageAtStart(lifted::Context& c) {
    ps2host::GuestMemory().Write(0x004DFCA0u, 0xFu, 4);
    ps2host::GuestMemory().Write(0x004DFCA4u, GameLanguage(), 4);
    c.r[4] = 0xFFFFFFFFFF000006ull;     // lui $a0, 0xff00; ori $a0, $a0, 6 (sign-extended)
    F_00466F18(c);
}

// 0x00466E38(source): copies a save's 12 configuration bytes back to 0x004DFCA0 (the language with them - a US
// save always says English), then tells the sound driver the saved output mode (0x00466F18, which does not read
// the language). As it is, then the chosen language again: the setting wins over the save.
void ConfigurationFromSave(lifted::Context& c) {
    F_00466E38(c);
    ps2host::GuestMemory().Write(0x004DFCA4u, GameLanguage(), 4);
}

// 0x00466FB8: the address of IOP_MAIN's status block (its word at +4: bit 31 set while a command to the sound
// driver waits for its answer). 24 loops in the executable call it and branch back until bit 31 clears - the EE
// spinning for the IOP (scratchpad pollscan.py). The IOP ran only at interrupts meanwhile and answered a blank
// late: ZOE US's 66 ms holds every ~7 s in fights started in one (0x004375E8 sends three commands and polls;
// the heavy-blank sampler, toolkit 532935a, had 77 % of that blank there, the IOP given 17-40 k instructions
// instead of ~90 k; M4 2026-11-14). As it is, then - while the answer is still out - an IOP slice, its replies
// delivered (RunIopBetweenSteps; real time only), as the console's IOP runs beside the spinning EE.
void IopStatusPoll(lifted::Context& c) {
    F_00466FB8(c);
    if (ps2host::GuestMemory().Read(u32(c.r[2]) + 4u, 4) & 0x80000000u) ps2host::RunIopBetweenSteps();
}

}  // namespace

void RegisterDub();     // dub.cpp: the Japanese voices from the player's Japanese disc (M9)

void RegisterGameOverrides() {
    RegisterDub();
    // The subtitle language (see GameLanguage): an experiment of the language research (M9 groundwork,
    // 2026-11-13) - the game's own variable, set where the game sets it.
    ps2host::Override(0x00466388u, LanguageAtStart, "language index from the 'language' setting (US: English fixed)");
    ps2host::Override(0x00466E38u, ConfigurationFromSave, "a loaded save's configuration, the language setting kept");
    // The PC settings' LANGUAGE line (M7, the ports' rule): the US disc's subtitle languages (docs/languages_research.md).
    ps2host::SetLanguages({"en", "fr", "de", "it"});
    // DUB under LANGUAGE (M9, the maintainer 2026-11-20): only with the player's Japanese disc set in the launcher
    // (its second line, "disc_ja"); dub.cpp serves its voices at the next start.
    ps2host::SetDubs({"en", "ja"}, [] { return !ps2host::Setting("disc_ja").empty(); });

    // ASPECT RATIO 16:9 / 16:10 / 21:9 and FIELD OF VIEW (M7 step 5): the cameras' projection (CameraMatrix,
    // CameraView), 3D play wide and 4:3 screens between bars (ShowsWide).
    ps2host::Override(0x00450C40u, CameraMatrix, "a camera's matrices built for ASPECT RATIO / FIELD OF VIEW");
    ps2host::Override(0x00450F58u, CameraView, "a camera's matrices built again when the setting changed");
    ps2host::Override(0x004597F0u, Object3d, "3D drawn: the picture is the wide one");
    ps2host::SetWidescreen(ShowsWide);
    ps2host::Override(0x00459908u, HudSprite, "a HUD sprite's draw: placed for ASPECT RATIO (its edge or centre)");
    ps2host::Override(0x0045B668u, HudText, "a text object's draw (ASPECT RATIO, logged)");
    ps2host::Override(0x0045ABE0u, HudWindow, "a window container's draw (ASPECT RATIO, logged)");
    ps2host::Override(0x00282A20u, MenuLabel, "a menu label made (traced: ZOE_MENU_TRACE)");
    // The title OPTIONS screen: VIBRATION, SOUND, RECOMP OPTIONS (our pages), EXIT (M7 step 7, M9): see the
    // OPTIONS section above.
    ps2host::Override(0x00420E88u, OptionsFrame, "the OPTIONS screen each frame: its rows' texts, our pages");
    ps2host::Override(0x00422FF0u, OptionsCursor, "the OPTIONS cursor: four rows (CAPTION, DEFAULT passed), our pages");
    ps2host::Override(0x00420FA0u, OptionsPress, "the OPTIONS confirm: RECOMP OPTIONS opens our pages; triangle: default");
    ps2host::Override(0x00423240u, OptionsFrameA, "the OPTIONS frame drawn at the row shown");
    ps2host::Override(0x004235A0u, OptionsFrameB, "the OPTIONS frame drawn at the row shown");

    // ZOE's own IOP modules (Konami's, from cdrom0:\MODULES) run in the IOP interpreter for bring-up (tier E):
    // their protocols are the game's. Same set as the PAL port (games/zoe, M1 2026-11-06): start-up polls SIF
    // software register 8 for bit 1, which one of these sets once it runs. Shipped path (design 7d): these IRX
    // get translated ahead of time instead.
    ps2host::iop::RunModule("JSIFMAN.IRX");
    ps2host::iop::RunModule("CDBIOS.IRX");
    ps2host::iop::RunModule("NOUSBFS.IRX");
    ps2host::iop::RunModule("IOP_MAIN.IRX");

    // VU1 fast mode by default (M3, 2026-11-12): the scripted run port/runs/hangar.args (boot -> movies ->
    // cutscenes -> hangar, 33600 frames, --gpu) draws all 168 pictures the same as the exact VU
    // (regress.py --vu-fast), VU1 4.4% -> 3.6% of the run. --vu-exact turns it off.
    ps2host::SetVuFastDefault(true);

    // Idle loop (M3, 2026-11-12): the lowest-priority thread's endless stream poll, as in the PAL port
    // (games/zoe/port/overrides.cpp, 0x00435C78 there): `for (;;) F_00436818(stream)`, loop head 0x00435720.
    ps2host::AddIdleLoop(0x00435720u);

    // IOP waits (M4, 2026-11-14): see IopStatusPoll.
    ps2host::Override(0x00466FB8u, IopStatusPoll, "IOP_MAIN's status block: the IOP runs while the EE polls it");

    // The pipelines the game draws with, made while it starts (pipeline_keys.inc; 2026-11-18: a cold shader cache
    // took 8.3 s on one worker).
    ps2host::SetGpuPipelineHints(kPipelineKeys, sizeof kPipelineKeys / sizeof kPipelineKeys[0]);
}
