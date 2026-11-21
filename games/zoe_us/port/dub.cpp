// The Japanese dub (M9, docs/m9_design.md): with the "dub" setting "ja" and the player's Japanese disc set in the
// launcher ("disc_ja", SLPM-65019), the game reads a virtual ZOE.DAT - the US one with the entries of every voice
// scene that is on both discs (and the three movies whose pictures are text cards in the disc's language) pointing
// past the US image's end, where the engine serves the Japanese files' own sectors from the Japanese image
// (Disc::MapSectors). Nothing is copied or changed on either image; the US program runs as it is.
//
// ZOE.DAT's table (tools/m3/zoe_dat.py): 32-byte entries `name[16], a, b, flags, 0` (little endian); a folder (flags
// 0x0102): a = its entries, b = the sector (from ZOE.DAT's start) of its table - or, for the root's "stage" folder,
// a byte offset into the same table; a file: a = its sector, b = its size in bytes. The root's own entry and the
// "stage" entry keep count and offset in bytes 8-15 (inside the name field).

#include "ps2host/disc.h"
#include "ps2host/host.h"
#include "ps2host/memory.h"
#include "ps2host/settings.h"
#include "ps2platform/platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

void WarmOptionsMenu();         // overrides.cpp

namespace {

using ps2host::Disc;
using ps2host::kSectorSize;
using u8 = unsigned char;
using u32 = unsigned int;

constexpr u32 kFolder = 0x0102;

u32 Le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
void SetLe32(u8* p, u32 v) {
    p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24);
}

struct Entry {
    std::string path;           // "stage/bs1/bs1g7121.pss", lower case
    u32 table = 0, index = 0;   // its table's sector (from ZOE.DAT's start) and its place there
    u32 sector = 0, size = 0;
};

// One ZOE.DAT's tables: read once, kept by their sector (the copies the rewritten entries go into).
struct Archive {
    Disc* disc = nullptr;
    u32 base = 0, sectors = 0;  // ZOE.DAT on the image
    std::map<u32, std::vector<u8>> tables;
    std::vector<Entry> files;
    std::set<std::pair<u32, u32>> seen;

    const u8* Table(u32 sector, u32 entries) {
        std::vector<u8>& t = tables[sector];
        const std::size_t want = (std::size_t(entries) * 32 + kSectorSize - 1) / kSectorSize * kSectorSize;
        if (t.size() < want) {
            t.assign(want, 0);
            if (sector >= sectors || !disc->Read(base + sector, u32(want / kSectorSize), t.data())) return nullptr;
        }
        return t.data();
    }

    void Walk(u32 sector, u32 count, u32 skip, const std::string& path, int depth) {
        if (depth > 8 || count > 4096) return;
        if (!Table(sector, skip + count)) return;
        // (the copy may grow while walking a folder below: entries are read again from it each time)
        for (u32 i = skip; i < skip + count; ++i) {
            const u8* raw = tables[sector].data() + std::size_t(i) * 32;
            char name[17] = {};
            std::memcpy(name, raw, 16);
            u32 a = Le32(raw + 16), b = Le32(raw + 20);
            const u32 flags = Le32(raw + 24) & 0xFFFF;
            if (flags == kFolder && a == 0 && b == 0 && std::strlen(name) <= 8) {
                a = Le32(raw + 8);
                b = Le32(raw + 12);
            }
            if (!name[0]) continue;
            std::string lower = name;
            for (char& ch : lower) ch = char(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            if (flags == kFolder) {
                if (b < 0x1000 && b % 32 == 0) Walk(sector, a, b / 32, path + lower + "/", depth + 1);
                else Walk(b, a, 0, path + lower + "/", depth + 1);
            } else if (seen.insert({sector, i}).second) {
                files.push_back(Entry{path + lower, sector, i, a, b});
            }
        }
    }

    bool Open(Disc& d) {
        disc = &d;
        Disc::File file;
        if (!d.Find("\\ZOE.DAT;1", &file)) return false;
        base = file.lsn;
        sectors = u32((static_cast<unsigned long long>(file.size) + kSectorSize - 1) / kSectorSize);
        std::vector<u8> head(kSectorSize);
        if (!d.Read(base, 1, head.data())) return false;
        Walk(0, Le32(&head[8]) + 1, 0, "", 0);
        return !files.empty();
    }
};

// A movie with pictures (a video stream, 0xE0, in its first packs) - a voice scene has none
// (docs/languages_research.md section 4).
// (The pictures start after the subtitle and the first voice packs - up to ~40 KB in: 64 sectors looked at.)
bool HasVideo(Archive& a, const Entry& e) {
    std::vector<u8> head(64 * kSectorSize);
    const u32 n = std::min<u32>(64, (e.size + kSectorSize - 1) / kSectorSize);
    if (!n || !a.disc->Read(a.base + e.sector, n, head.data())) return false;
    for (std::size_t i = 0; i + 4 <= std::size_t(n) * kSectorSize; ++i)
        if (head[i] == 0 && head[i + 1] == 0 && head[i + 2] == 1 && head[i + 3] == 0xE0) return true;
    return false;
}

// The subtitles of a voice scene (docs/languages_research.md section 6, m9_design.md decision 1 (a)). A voice .pss
// is an MPEG program stream in 4096-byte units, each one private-stream-1 packet (0xBD, sub-stream 0xFF) whose
// payload starts `ff 90 00 <channel>`: channel 1 the voice, 0 the Japanese subtitles (JP disc), 7-10 the
// English/French/German/Italian ones (US disc), each a block `count u32, bytes u32, u32, u32` and per cue
// `start u32, end u32, text bytes u32, flags u32, text[text bytes]`. The US units with the subtitles come first.
// The Japanese dub's scene is served as the US file's subtitle units (their cue times moved onto the Japanese
// voice) followed by the whole Japanese file: the US program reads channels 7-10 and skips 0 and the rest.
// A block longer than one packet goes on in the channel's next packet (each starting `ff 90 00 <channel>` again,
// another channel's packets possibly between): a channel's data is its packets' payloads after those 4 bytes, in
// file order; `at` maps each of its bytes to its place in the buffer.
struct Cue {
    std::size_t at;             // the cue's place in its channel's data (its `start`)
    u32 start, end;
};

struct Channel {
    u32 number = 0;
    std::vector<std::size_t> at;
    std::vector<Cue> cues;
    u32 Get(const std::vector<u8>& d, std::size_t k) const {
        return u32(d[at[k]]) | u32(d[at[k + 1]]) << 8 | u32(d[at[k + 2]]) << 16 | u32(d[at[k + 3]]) << 24;
    }
    void Put(std::vector<u8>& d, std::size_t k, u32 v) const {
        for (int b = 0; b < 4; ++b) d[at[k + b]] = u8(v >> (8 * b));
    }
};

struct Subtitles {
    std::size_t end = 0;        // where the leading subtitle packets end (0: none found)
    std::vector<Channel> channels;
    bool ok = true;             // every block read whole
};

// The leading subtitle packets of a scene's first bytes (up to its first other packet); `wanted` the channels
// counted as subtitles.
Subtitles ReadSubtitles(const std::vector<u8>& d, bool (*wanted)(u32)) {
    Subtitles out;
    std::size_t i = 0;
    while (i + 6 <= d.size()) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) break;        // only whole packets, back to back
        const u8 id = d[i + 3];
        if (id == 0xBA) {                                               // pack header
            if (i + 14 > d.size()) break;
            i += 14 + (d[i + 13] & 7);
            continue;
        }
        if (id < 0xBB) break;
        const std::size_t len = std::size_t(d[i + 4]) << 8 | d[i + 5], body = i + 6;
        if (body + len > d.size()) break;
        if (id == 0xBD) {
            const std::size_t pl = body + 3 + d[body + 2];
            if (pl + 4 > body + len || d[pl] != 0xFF || d[pl + 1] != 0x90 || d[pl + 2] != 0) break;
            const u32 number = d[pl + 3];
            if (!wanted(number)) break;                                 // the voice / the pictures: over
            auto c = std::find_if(out.channels.begin(), out.channels.end(),
                                  [&](const Channel& x) { return x.number == number; });
            if (c == out.channels.end()) {
                out.channels.push_back(Channel{number, {}, {}});
                c = out.channels.end() - 1;
            }
            for (std::size_t k = pl + 4; k < body + len; ++k) c->at.push_back(k);
        } else if (id != 0xBE && id != 0xBB) {
            break;
        }
        i = body + len;
        if (!out.channels.empty()) out.end = i;
    }
    for (Channel& c : out.channels) {
        if (c.at.size() < 16) { out.ok = false; continue; }
        const u32 count = c.Get(d, 0);
        std::size_t k = 16;
        for (u32 n = 0; n < count; ++n) {
            if (n >= 512 || k + 16 > c.at.size()) { out.ok = false; break; }
            c.cues.push_back(Cue{k, c.Get(d, k), c.Get(d, k + 4)});
            k += 16 + c.Get(d, k + 8);
        }
    }
    return out;
}

bool UsSubtitleChannel(u32 channel) { return channel >= 7 && channel <= 10; }
bool JaSubtitleChannel(u32 channel) { return channel == 0; }

// How a scene's English cues are put onto the Japanese voice (2026-11-20 issue 6; the "dub_subs" setting, or
// "dub_subs_<scene>" for one scene - its file name without .pss):
//   auto (default) - a movie (pictures: the same picture time on both discs) keeps the US times, each line then
//     snapped to the Japanese lines it overlaps; a voice scene with as many Japanese lines takes their times one
//     for one, else the English span stretched onto the Japanese one and snapped the same way;
//   jp - v0.8.0's way (as many: one for one; else stretched; no Japanese lines: by the files' sizes) - the opening
//     (fa1r1020: two Japanese-only lines first) came far too early so;
//   us - the US times unchanged.
enum class SubsMode { kAuto, kJp, kUs };

SubsMode ModeOf(const std::string& scene) {
    std::string v = ps2host::Setting("dub_subs_" + scene.substr(0, scene.rfind('.')));
    if (v.empty()) v = ps2host::Setting("dub_subs", "auto");
    return v == "jp" ? SubsMode::kJp : v == "us" ? SubsMode::kUs : SubsMode::kAuto;
}

// The English lines onto the Japanese ones (tools/m9/subs_survey.py's survey: 437 scenes, 13 left with a line
// under 0.5 s - their Japanese voice is that much shorter): a Japanese line that a quarter or more of two or more
// English lines falls on is shared out among them by their lengths; any other goes to the English line it overlaps
// most, which then runs from its first Japanese line's start to its last's end; a line with none keeps its time;
// then lines kept apart (one ends where the next starts).
// (`reading`: each English line's own length on the US disc, before any stretch.)
std::vector<std::pair<double, double>> Snap(std::vector<std::pair<double, double>> en, const std::vector<Cue>& ja,
                                            const std::vector<double>& reading) {
    const std::size_t n = en.size();
    std::vector<std::pair<double, double>> out = en;
    std::vector<bool> done(n, false);
    std::vector<int> owner(ja.size(), -1);
    auto overlap = [&](std::size_t e, std::size_t j) {
        return std::min<double>(en[e].second, ja[j].end) - std::max<double>(en[e].first, ja[j].start);
    };
    for (std::size_t j = 0; j < ja.size(); ++j) {
        std::vector<std::size_t> shared;
        for (std::size_t e = 0; e < n; ++e)
            if (overlap(e, j) > 0.25 * (en[e].second - en[e].first)) shared.push_back(e);
        if (shared.size() >= 2) {
            double total = 0.0;
            for (std::size_t e : shared) total += en[e].second - en[e].first;
            double at = ja[j].start;
            // Over the line and the pause after it (up to 2 frames before the next Japanese line, a second at most):
            // two English lines on one quicker Japanese one ran too fast (his session 2026-11-21).
            const double next = j + 1 < ja.size() ? double(ja[j + 1].start) - 2.0 : double(ja[j].end) + 60.0;
            const double until = std::max(double(ja[j].end), std::min(next, double(ja[j].end) + 60.0));
            for (std::size_t e : shared) {
                const double span = until - ja[j].start;
                const double part = total > 0.0 ? span * (en[e].second - en[e].first) / total : span / shared.size();
                out[e] = {at, at + part};
                at += part;
                done[e] = true;
            }
            continue;
        }
        double best = 0.0;
        for (std::size_t e = 0; e < n; ++e)
            if (overlap(e, j) > best) best = overlap(e, j), owner[j] = int(e);
    }
    for (std::size_t e = 0; e < n; ++e) {
        if (done[e]) continue;
        double s = 1e18, t = -1e18;
        for (std::size_t j = 0; j < ja.size(); ++j)
            if (owner[j] == int(e)) s = std::min<double>(s, ja[j].start), t = std::max<double>(t, ja[j].end);
        if (t > s) out[e] = {s, t};
    }
    for (std::size_t e = 1; e < n; ++e)
        if (out[e].first < out[e - 1].second) out[e - 1].second = std::max(out[e - 1].first + 1.0, out[e].first);
    (void)reading;
    return out;
}

// Read in time: a line stays as long as its English one did (its reading time), as far as the next line lets it
// (2 frames before) - shared and snapped lines had run too fast (the mech's first scene, fa1c1081: 41 frames for a
// line of 59; his session 2026-11-21).
void ReadInTime(std::vector<std::pair<double, double>>* lines, const std::vector<double>& reading) {
    std::vector<std::pair<double, double>>& out = *lines;
    for (std::size_t e = 0; e < out.size(); ++e) {
        const double want = out[e].first + reading[e];
        const double room = e + 1 < out.size() ? out[e + 1].first - 2.0 : want;
        if (out[e].second < want) out[e].second = std::max(out[e].second, std::min(want, room));
    }
}

// One language's cues moved onto the Japanese voice (SubsMode). The English lines in time order; with as many
// Japanese ones, theirs one for one (both in time order) - unless a movie in auto.
void Retime(std::vector<u8>& d, const Channel& channel, std::vector<Cue> ja, u32 us_size, u32 ja_size, bool movie,
            SubsMode mode) {
    if (channel.cues.empty() || mode == SubsMode::kUs) return;
    if (mode == SubsMode::kAuto) {
        std::vector<Cue> order = channel.cues;
        std::sort(order.begin(), order.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
        std::sort(ja.begin(), ja.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
        std::vector<std::pair<double, double>> en;
        std::vector<double> reading;
        for (const Cue& c : order) {
            en.push_back({double(c.start), double(c.end)});
            reading.push_back(double(c.end) - double(c.start));
        }
        if (!ja.empty()) {
            if (!movie && ja.size() == order.size()) {
                for (std::size_t k = 0; k < en.size(); ++k) en[k] = {double(ja[k].start), double(ja[k].end)};
            } else {
                if (!movie) {           // the English span onto the Japanese span, then snapped
                    double us_first = en.front().first, us_last = 0, ja_last = 0;
                    for (const auto& c : en) us_last = std::max(us_last, c.second);
                    for (const Cue& c : ja) ja_last = std::max<double>(ja_last, c.end);
                    const double scale = us_last > us_first ? (ja_last - ja.front().start) / (us_last - us_first) : 1.0;
                    for (auto& c : en)
                        c = {ja.front().start + (c.first - us_first) * scale, ja.front().start + (c.second - us_first) * scale};
                }
                en = Snap(en, ja, reading);
            }
            ReadInTime(&en, reading);
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            u32 s = u32(std::max(0.0, en[k].first) + 0.5), e = u32(std::max(0.0, en[k].second) + 0.5);
            if (e <= s) e = s + 1;
            channel.Put(d, order[k].at, s);
            channel.Put(d, order[k].at + 4, e);
        }
        return;
    }
    std::vector<Cue> order = channel.cues;
    std::sort(order.begin(), order.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    std::sort(ja.begin(), ja.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    auto put = [&](const Cue& c, double start, double end) {
        u32 s = u32(start + 0.5), e = u32(end + 0.5);
        if (e <= s) e = s + 1;
        channel.Put(d, c.at, s);
        channel.Put(d, c.at + 4, e);
    };
    if (ja.size() == order.size()) {
        for (std::size_t k = 0; k < order.size(); ++k) put(order[k], ja[k].start, ja[k].end);
        return;
    }
    u32 us_first = order.front().start, us_last = 0;
    for (const Cue& c : order) us_last = std::max(us_last, c.end);
    double from = 0, scale = us_size ? double(ja_size) / us_size : 1.0;
    if (!ja.empty() && us_last > us_first) {
        u32 ja_last = 0;
        for (const Cue& c : ja) ja_last = std::max(ja_last, c.end);
        scale = double(ja_last - ja.front().start) / double(us_last - us_first);
        from = ja.front().start;
    } else {
        us_first = 0;
    }
    for (const Cue& c : order) put(c, from + (c.start - us_first) * scale, from + (c.end - us_first) * scale);
}

// The US scene's subtitle units with their cues on the Japanese voice, padded to whole 16384-byte packs (the
// stream's pack interval; a padding packet fills the gap) - empty when the scene's layout is not the known one.
// ---- Fewer Japanese lines than English (the maintainer's idea 2026-11-21): the Japanese times exactly, the English
// lines put together onto them - consecutive lines a Japanese one each, the groups chosen where the lines' starts fit
// best (each start's share of the scene and its length's share); a group's text its lines one under the other. The subtitle units are written anew (the texts' lengths change): the same pack - its header,
// each language's block in its own 4096-byte unit, padding between - so the scene's mapping stays as it was.
struct TextLine {
    u32 start = 0, end = 0, flags = 0;
    std::string text;
};

std::vector<TextLine> ChannelLines(const std::vector<u8>& d, const Channel& c) {
    std::vector<TextLine> out;
    for (const Cue& cue : c.cues) {
        TextLine l;
        l.start = c.Get(d, cue.at);
        l.end = c.Get(d, cue.at + 4);
        const u32 bytes = c.Get(d, cue.at + 8);
        l.flags = c.Get(d, cue.at + 12);
        for (u32 i = 0; i < bytes && cue.at + 16 + i < c.at.size(); ++i) {
            const u8 ch = d[c.at[cue.at + 16 + i]];
            if (!ch) break;
            l.text += char(ch);
        }
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const TextLine& a, const TextLine& b) { return a.start < b.start; });
    return out;
}

// The English lines grouped onto the Japanese ones (n > m): group j = lines [cut[j], cut[j+1]).
std::vector<std::size_t> GroupLines(const std::vector<TextLine>& en, std::vector<Cue> ja) {
    std::sort(ja.begin(), ja.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    const std::size_t n = en.size(), m = ja.size();
    double en_last = 0, ja_last = 0, total_en = 0, total_ja = 0;
    for (const TextLine& l : en) en_last = std::max<double>(en_last, l.end), total_en += double(l.end) - l.start;
    for (const Cue& c : ja) ja_last = std::max<double>(ja_last, c.end), total_ja += double(c.end) - c.start;
    if (total_en <= 0.0) total_en = 1.0;
    if (total_ja <= 0.0) total_ja = 1.0;
    auto share_en = [&](std::size_t i) {
        return en_last > en.front().start ? (double(en[i].start) - en.front().start) / (en_last - en.front().start) : 0.0;
    };
    auto share_ja = [&](std::size_t j) {
        return ja_last > ja.front().start ? (double(ja[j].start) - ja.front().start) / (ja_last - ja.front().start) : 0.0;
    };
    // cost[j][i]: the best for Japanese lines 0..j-1 taking English lines 0..i-1 (each at least one).
    const double kInf = 1e18;
    std::vector<std::vector<double>> cost(m + 1, std::vector<double>(n + 1, kInf));
    std::vector<std::vector<std::size_t>> from(m + 1, std::vector<std::size_t>(n + 1, 0));
    cost[0][0] = 0.0;
    for (std::size_t j = 1; j <= m; ++j)
        for (std::size_t i = j; i <= n - (m - j); ++i)
            for (std::size_t k = j - 1; k < i; ++k) {       // group j-1 = English lines k..i-1
                if (cost[j - 1][k] >= kInf) continue;
                // where it starts, and how much of the scene it takes - twice as much (fa1c1081: "Who are you!?" on
                // the short Japanese line, "I am ADA ..." on the long one)
                double group = 0.0;
                for (std::size_t x = k; x < i; ++x) group += double(en[x].end) - en[x].start;
                const double d = share_en(k) - share_ja(j - 1);
                const double t = group / total_en - (double(ja[j - 1].end) - ja[j - 1].start) / total_ja;
                const double c = cost[j - 1][k] + d * d + 2.0 * t * t;
                if (c < cost[j][i]) cost[j][i] = c, from[j][i] = k;
            }
    std::vector<std::size_t> cut(m + 1, 0);
    cut[m] = n;
    for (std::size_t j = m, i = n; j > 0; --j) {
        cut[j - 1] = from[j][i];
        i = cut[j - 1];
    }
    return cut;
}

// A subtitle pack written anew: its own header (up to the first subtitle packet), then each language's packet
// (subs.channels' order, `lines` each) in its 4096-byte unit - a longer one takes more units; the last one ends at
// subs.end at least. Empty: a block over a packet's 64 KiB.
std::vector<u8> WritePack(const std::vector<u8>& prefix, const Subtitles& subs,
                          const std::vector<std::vector<TextLine>>& lines) {
    constexpr std::size_t kUnit = 4096;
    std::size_t head = 0;
    while (head + 4 <= prefix.size() && !(prefix[head] == 0 && prefix[head + 1] == 0 && prefix[head + 2] == 1 &&
                                          prefix[head + 3] == 0xBD))
        ++head;
    if (head + 9 > prefix.size()) return {};
    const std::size_t pes = std::size_t(3) + prefix[head + 8];          // the first packet's PES header
    std::vector<u8> out(prefix.begin(), prefix.begin() + std::ptrdiff_t(head));
    for (std::size_t k = 0; k < subs.channels.size() && k < lines.size(); ++k) {
        std::vector<u8> block(16, 0);
        u32 bytes = 0;
        for (const TextLine& t : lines[k]) {
            const u32 room = (u32(t.text.size()) + 4u) & ~3u;
            u8 cue[16];
            const u32 words[4] = {t.start, t.end, room, t.flags};
            for (int w = 0; w < 4; ++w)
                for (int b = 0; b < 4; ++b) cue[w * 4 + b] = u8(words[w] >> (8 * b));
            block.insert(block.end(), cue, cue + 16);
            std::vector<u8> text(room, 0);
            std::memcpy(text.data(), t.text.data(), t.text.size());
            block.insert(block.end(), text.begin(), text.end());
            bytes += 16 + room;
        }
        SetLe32(&block[0], u32(lines[k].size()));
        SetLe32(&block[4], bytes);
        const std::size_t len = pes + 4 + block.size();
        if (len > 0xFFFF) return {};
        std::size_t unit_end = (out.size() + 6 + len + 6 + kUnit - 1) / kUnit * kUnit;
        if (k + 1 == subs.channels.size()) unit_end = std::max(unit_end, subs.end);
        const u8 bd[6] = {0, 0, 1, 0xBD, u8(len >> 8), u8(len)};
        out.insert(out.end(), bd, bd + 6);
        out.insert(out.end(), prefix.begin() + std::ptrdiff_t(head + 6), prefix.begin() + std::ptrdiff_t(head + 6 + pes));
        const u8 sub[4] = {0xFF, 0x90, 0x00, u8(subs.channels[k].number)};
        out.insert(out.end(), sub, sub + 4);
        out.insert(out.end(), block.begin(), block.end());
        const std::size_t pad = unit_end - out.size() - 6;
        const u8 be[6] = {0, 0, 1, 0xBE, u8(pad >> 8), u8(pad)};
        out.insert(out.end(), be, be + 6);
        out.resize(unit_end, 0xFF);
    }
    return out;
}

// The subtitle pack written anew with the merged lines (empty: no language needs it, or it does not fit).
// (`original`: the US file's own pack - the lines grouped by their own times; `prefix`: as Retime left it - a
// language not merged keeps those.)
std::vector<u8> MergedPrefix(const std::vector<u8>& original, const std::vector<u8>& prefix, const Subtitles& subs,
                             const std::vector<Cue>& ja) {
    if (ja.empty() || subs.channels.empty()) return {};
    bool any = false;
    std::vector<std::vector<TextLine>> lines;
    for (const Channel& c : subs.channels) {
        lines.push_back(ChannelLines(original, c));
        if (lines.back().size() > ja.size()) any = true;
        else lines.back() = ChannelLines(prefix, c);
    }
    if (!any) return {};
    std::vector<Cue> jas = ja;
    std::sort(jas.begin(), jas.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    for (std::vector<TextLine>& l : lines) {
        if (l.size() <= jas.size()) continue;                       // (as Retime left it)
        const std::vector<std::size_t> cut = GroupLines(l, jas);
        std::vector<TextLine> merged;
        for (std::size_t j = 0; j < jas.size(); ++j) {
            TextLine g;
            g.start = jas[j].start;
            g.end = jas[j].end;
            g.flags = l[cut[j]].flags;
            for (std::size_t i = cut[j]; i < cut[j + 1]; ++i) {
                if (!g.text.empty()) g.text += '\n';
                g.text += l[i].text;
            }
            merged.push_back(g);
        }
        l = merged;
    }
    std::vector<u8> out = WritePack(prefix, subs, lines);
    return out.size() == subs.end ? out : std::vector<u8>{};
}

// ---- The subtitle pack: our own translations, a download of its own beside ZOE.exe (the maintainer 2026-11-21:
// "die eigenen Übersetzungen sollen auch aus der exe, dafür ist der separate Download" - how many fetch it shows
// the demand). tools/m10/subs_translation.py builds it from translation/*.json: the Japanese voices' lines
// translated from the Japanese (EN / FR / DE / IT / ES in the US file's channels 7 / 8 / 9 / 10, the Japanese
// line's start, end and speaker), Spanish and Japanese with the English voices, the Spanish descriptions and stage
// script texts, and Noto Sans JP for the Japanese subtitles. Each line found by its source text's FNV-1a hash - no
// disc text in it. Without the pack: the disc's four languages, the Japanese voices with them moved onto the
// Japanese timing (v0.8.0's way); SPANISH and JAPANESE are not offered.
struct TranslatedLine {
    u64 hash;
    const char* text[5];
};
struct SpanishLine {            // (also the Japanese ones from the English: UTF-8)
    u64 hash;
    const char* text;
};
struct SpanishDescription {
    u32 at, group;
    u64 hash;
    const char* text;
};

struct SubtitlePack {
    bool loaded = false;
    std::string path;
    std::deque<std::string> texts;              // (the lines' bytes; a deque keeps the pointers)
    std::vector<TranslatedLine> translated;     // sorted by hash, as written
    std::vector<SpanishLine> es_from_en, ja_from_en, scripts;
    std::vector<SpanishDescription> descriptions;
    std::vector<u8> font;
};

class PackReader {
public:
    PackReader(const u8* p, std::size_t n) : p_(p), n_(n) {}
    bool Ok() const { return ok_; }
    bool Done() const { return at_ >= n_; }
    u32 U32() { return u32(Bytes(4, [](const u8* b) { return u64(b[0]) | u64(b[1]) << 8 | u64(b[2]) << 16 | u64(b[3]) << 24; })); }
    u64 U64() {
        const u64 lo = U32();
        return lo | u64(U32()) << 32;
    }
    const char* Text(std::deque<std::string>& keep) {
        const u32 len = u32(Bytes(2, [](const u8* b) { return u64(b[0]) | u64(b[1]) << 8; }));
        if (!ok_ || at_ + len > n_) {
            ok_ = false;
            return "";
        }
        keep.emplace_back(reinterpret_cast<const char*>(p_ + at_), len);
        at_ += len;
        return keep.back().c_str();
    }
private:
    template <class F>
    u64 Bytes(std::size_t k, F f) {
        if (!ok_ || at_ + k > n_) {
            ok_ = false;
            return 0;
        }
        const u64 v = f(p_ + at_);
        at_ += k;
        return v;
    }
    const u8* p_;
    std::size_t n_, at_ = 0;
    bool ok_ = true;
};

// The pack's place: the setting "subtitle_pack" (the launcher's line), else ZOE-subtitles.pak beside the game or
// in its "game" folder.
SubtitlePack ReadSubtitlePack() {
    SubtitlePack pack;
    std::vector<std::filesystem::path> places;
    const std::string set = ps2host::Setting("subtitle_pack");
    if (!set.empty()) places.push_back(std::filesystem::u8path(set));
    const std::filesystem::path here = ps2platform::ExeDir();
    // (also without its extension: v0.8.2's launcher copied it so - by its content, the magic below)
    for (const std::filesystem::path& folder : {here / "game", here})
        for (const char* name : {"ZOE-subtitles.pak", "ZOE-subtitles"}) places.push_back(folder / name);
    std::vector<u8> file;
    for (const std::filesystem::path& p : places) {
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        file.resize(size > 0 ? std::size_t(size) : 0u);
        const bool read = !file.empty() && std::fread(file.data(), 1, file.size(), f) == file.size();
        std::fclose(f);
        if (read && file.size() > 8 && std::memcmp(file.data(), "ZOESUBS\x01", 8) == 0) {
            pack.path = p.string();
            break;
        }
        std::fprintf(stderr, "[subtitles] %s is no subtitle pack\n", p.string().c_str());
        file.clear();
    }
    if (pack.path.empty()) {
        std::fprintf(stderr, "[subtitles] no subtitle pack (ZOE-subtitles.pak): the disc's subtitles only\n");
        return pack;
    }
    bool ok = true;
    // Sections: a 4-letter tag, its size (u32 LE), its bytes.
    std::size_t at = 8;
    while (ok && at + 8 <= file.size()) {
        const std::string tag(reinterpret_cast<const char*>(&file[at]), 4);
        const u32 size = u32(file[at + 4]) | u32(file[at + 5]) << 8 | u32(file[at + 6]) << 16 | u32(file[at + 7]) << 24;
        at += 8;
        if (at + size > file.size()) {
            ok = false;
            break;
        }
        PackReader r(&file[at], size);
        if (tag == "JPTR") {
            for (u32 n = r.U32(); r.Ok() && n--;) {
                TranslatedLine l{};
                l.hash = r.U64();
                for (const char*& t : l.text) t = r.Text(pack.texts);
                pack.translated.push_back(l);
            }
        } else if (tag == "ESEN" || tag == "JAEN" || tag == "SCXE") {
            std::vector<SpanishLine>& to = tag == "ESEN" ? pack.es_from_en : tag == "JAEN" ? pack.ja_from_en : pack.scripts;
            for (u32 n = r.U32(); r.Ok() && n--;) {
                SpanishLine l{};
                l.hash = r.U64();
                l.text = r.Text(pack.texts);
                to.push_back(l);
            }
        } else if (tag == "DESC") {
            for (u32 n = r.U32(); r.Ok() && n--;) {
                SpanishDescription d{};
                d.at = r.U32();
                d.group = r.U32();
                d.hash = r.U64();
                d.text = r.Text(pack.texts);
                pack.descriptions.push_back(d);
            }
        } else if (tag == "FONT") {
            pack.font.assign(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(at + size));
        }
        ok = r.Ok();
        at += size;
    }
    auto sorted = [](const auto& v) {
        return std::is_sorted(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.hash < b.hash; });
    };
    if (!ok || !sorted(pack.translated) || !sorted(pack.es_from_en) || !sorted(pack.ja_from_en) || !sorted(pack.scripts)) {
        std::fprintf(stderr, "[subtitles] %s is damaged: the disc's subtitles only\n", pack.path.c_str());
        return SubtitlePack{};
    }
    pack.loaded = true;
    std::fprintf(stderr, "[subtitles] %s: %zu lines from the Japanese, %zu Spanish and %zu Japanese from the English, "
                 "%zu descriptions, %zu script texts, font %zu bytes\n", pack.path.c_str(), pack.translated.size(),
                 pack.es_from_en.size(), pack.ja_from_en.size(), pack.descriptions.size(), pack.scripts.size(),
                 pack.font.size());
    return pack;
}

const SubtitlePack& Pack() {
    static const SubtitlePack pack = ReadSubtitlePack();
    return pack;
}

template <class T>
const T* FindByHash(const std::vector<T>& v, u64 h) {
    const auto at = std::lower_bound(v.begin(), v.end(), h, [](const T& l, u64 x) { return l.hash < x; });
    return at != v.end() && at->hash == h ? &*at : nullptr;
}

u64 Fnv1a(const std::string& s) {
    u64 h = 0xcbf29ce484222325ull;
    for (char c : s) h = (h ^ u8(c)) * 0x100000001b3ull;
    return h;
}

// SUBTITLE LANGUAGE SPANISH (the maintainer 2026-11-21: a fifth language, ours; with the pack): the game runs in
// English, its channel 7 then carries the Spanish lines.
bool SpanishChosen() { return Pack().loaded && ps2host::Setting("language") == "es"; }

// SUBTITLE LANGUAGE JAPANESE (the maintainer 2026-11-21; with the pack): the game's letters have no Japanese, so
// its channel 7 carries "#J" and the line's hash, and the text object's draw (overrides.cpp JapaneseSubtitle)
// draws the line in Noto Sans JP instead - with the Japanese voices the Japanese disc's own line, with the English
// voices our Japanese from the English.
bool JapaneseChosen() { return Pack().loaded && ps2host::Setting("language") == "ja"; }

std::map<u64, std::string>& JapaneseTexts() {
    static std::map<u64, std::string> lines;     // hash -> the line in UTF-8
    return lines;
}

// EUC-JP (the Japanese disc's subtitles) to UTF-8.
std::string Utf8FromEucJp(const std::string& euc) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(20932, 0, euc.data(), int(euc.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(std::size_t(n), L'\0');
    MultiByteToWideChar(20932, 0, euc.data(), int(euc.size()), wide.data(), n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(m > 0 ? m : 0), '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, out.data(), m, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "EUC-JP");
    if (cd == iconv_t(-1)) return {};
    std::string in = euc, out(euc.size() * 2 + 16, '\0');
    char* src = in.data();
    char* dst = out.data();
    std::size_t left = in.size(), room = out.size();
    iconv(cd, &src, &left, &dst, &room);
    iconv_close(cd);
    out.resize(out.size() - room);
    return out;
#endif
}

const TranslatedLine* Translated(const std::string& japanese) { return FindByHash(Pack().translated, Fnv1a(japanese)); }

// The Japanese lines: the scene's subtitle channel with the most lines that say something (0; some scenes 5 or 1).
std::vector<TextLine> JapaneseLines(const std::vector<u8>& j) {
    const Subtitles subs = ReadSubtitles(j, [](u32 c) { return c <= 5u; });
    std::vector<TextLine> best;
    for (const Channel& c : subs.channels) {
        std::vector<TextLine> lines;
        for (TextLine& l : ChannelLines(j, c))
            if (l.end > l.start && l.end < 216000u &&                  // (a sane time: under an hour)
                l.text.find_first_not_of(" \n") != std::string::npos)
                lines.push_back(std::move(l));
        if (lines.size() > best.size()) best.swap(lines);
    }
    return best;
}

// The pack with the translations (empty: a Japanese line without one - the old way then).
std::vector<u8> TranslatedPrefix(const std::vector<u8>& prefix, const Subtitles& us_subs, const std::vector<u8>& j) {
    const std::vector<TextLine> ja = JapaneseLines(j);
    if (ja.empty()) return {};
    std::vector<std::vector<TextLine>> lines(us_subs.channels.size());
    for (const TextLine& l : ja) {
        const TranslatedLine* t = Translated(l.text);
        if (!t) return {};
        for (std::size_t k = 0; k < us_subs.channels.size(); ++k) {
            u32 lang = us_subs.channels[k].number - 7u;                 // 7 EN, 8 FR, 9 DE, 10 IT
            if (lang > 3u) return {};
            if (lang == 0u && SpanishChosen()) lang = 4u;                 // (ES in channel 7)
            if (lang == 0u && JapaneseChosen()) {                           // (the marker; the line kept)
                char marker[24];
                std::snprintf(marker, sizeof marker, "#J%016llx", (unsigned long long)Fnv1a(l.text));
                JapaneseTexts()[Fnv1a(l.text)] = Utf8FromEucJp(l.text);
                lines[k].push_back(TextLine{l.start, l.end, l.flags, marker});
                continue;
            }
            // An empty translation: a noise (a gasp, a laugh - the maintainer 2026-11-21: not shown, "das ist
            // natürlicher"): no subtitle there.
            if (!*t->text[lang]) continue;
            // ZOE_SUB_TEST=<text> (diagnosis: the game's letters): every translated line that text
            static const char* test = std::getenv("ZOE_SUB_TEST");
            lines[k].push_back(TextLine{l.start, l.end, l.flags, test ? std::string(test) : std::string(t->text[lang])});
        }
    }
    return WritePack(prefix, us_subs, lines);
}

// With the English voices and SPANISH or JAPANESE: a scene's pack with channel 7 alone, its lines in Spanish (or as
// "#J" markers of our Japanese) at their own times (an English line without one stays English); empty: it does not
// fit the file's own pack.
std::vector<u8> EnglishVoicesPrefix(const std::vector<u8>& u, const Subtitles& us_subs, bool japanese) {
    const auto seven = std::find_if(us_subs.channels.begin(), us_subs.channels.end(),
                                    [](const Channel& c) { return c.number == 7u; });
    if (seven == us_subs.channels.end()) return {};
    Subtitles only = us_subs;
    only.channels.assign(1, *seven);
    std::vector<std::vector<TextLine>> lines(1, ChannelLines(u, *seven));
    std::vector<TextLine> kept;
    for (TextLine& l : lines[0]) {
        const u64 h = Fnv1a(l.text);
        const SpanishLine* own = FindByHash(japanese ? Pack().ja_from_en : Pack().es_from_en, h);
        if (own && !*own->text) continue;               // a noise: no subtitle there (see TranslatedPrefix)
        if (own && !japanese) {
            l.text = own->text;
        } else if (own) {
            char marker[24];
            std::snprintf(marker, sizeof marker, "#J%016llx", (unsigned long long)h);
            JapaneseTexts()[h] = own->text;
            l.text = marker;
        }
        kept.push_back(std::move(l));
    }
    lines[0].swap(kept);
    const std::vector<u8> prefix(u.begin(), u.begin() + std::ptrdiff_t(us_subs.end));
    std::vector<u8> out = WritePack(prefix, only, lines);
    return out.size() == us_subs.end ? out : std::vector<u8>{};
}

std::vector<u8> SubtitlePrefix(Archive& us_dat, const Entry& us, Archive& ja_dat, const Entry& ja, bool movie,
                               SubsMode mode) {
    constexpr std::size_t kPack = 16384;
    std::vector<u8> u(std::min<std::size_t>(64 * kSectorSize, (us.size + kSectorSize - 1) / kSectorSize * kSectorSize));
    std::vector<u8> j(std::min<std::size_t>(32 * kSectorSize, (ja.size + kSectorSize - 1) / kSectorSize * kSectorSize));
    if (u.empty() || j.empty() || !us_dat.disc->Read(us_dat.base + us.sector, u32(u.size() / kSectorSize), u.data()) ||
        !ja_dat.disc->Read(ja_dat.base + ja.sector, u32(j.size() / kSectorSize), j.data()))
        return {};
    const Subtitles us_subs = ReadSubtitles(u, UsSubtitleChannel);
    if (!us_subs.end || !us_subs.ok) return {};
    const Subtitles ja_subs = ReadSubtitles(j, JaSubtitleChannel);
    const std::vector<Cue> ja_cues =
        ja_subs.ok && !ja_subs.channels.empty() ? ja_subs.channels.front().cues : std::vector<Cue>{};
    std::vector<u8> prefix(u.begin(), u.begin() + us_subs.end);
    std::vector<u8> translated = mode == SubsMode::kAuto ? TranslatedPrefix(prefix, us_subs, j) : std::vector<u8>{};
    const bool from_japanese = !translated.empty();
    if (from_japanese) {
        prefix.swap(translated);
        mode = SubsMode::kUs;               // (nothing more to do below)
    }
    if (!from_japanese)
        for (const Channel& channel : us_subs.channels) Retime(prefix, channel, ja_cues, us.size, ja.size, movie, mode);
    if (mode == SubsMode::kAuto && !movie) {        // fewer Japanese lines: English ones put together on them
        const std::vector<u8> original(u.begin(), u.begin() + std::ptrdiff_t(us_subs.end));
        std::vector<u8> merged = MergedPrefix(original, prefix, us_subs, ja_cues);
        if (!merged.empty()) {
            prefix.swap(merged);
            // ZOE_DUB_DUMP=<folder>: each merged scene's new subtitle pack, <scene>.bin (diagnosis)
            if (const char* dump = std::getenv("ZOE_DUB_DUMP")) {
                const std::string name = us.path.substr(us.path.rfind('/') + 1);
                if (std::FILE* f = std::fopen((std::string(dump) + "/" + name + ".bin").c_str(), "wb")) {
                    std::fwrite(prefix.data(), 1, prefix.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
    std::size_t padded = (prefix.size() + kPack - 1) / kPack * kPack;
    if (padded != prefix.size() && padded - prefix.size() < 6) padded += kPack;
    if (padded != prefix.size()) {
        const std::size_t len = padded - prefix.size() - 6;
        if (len > 0xFFFF) return {};
        const u8 head[6] = {0, 0, 1, 0xBE, u8(len >> 8), u8(len)};
        prefix.insert(prefix.end(), head, head + 6);
        prefix.resize(padded, 0xFF);
    }
    return prefix;
}

Disc& JapaneseDisc() {
    static Disc disc;           // read for the whole run (its sectors are served from it)
    return disc;
}

// The two ZOE.DATs' tables, kept for the run (a scene's subtitles made again: SUBTITLE TIMING, below).
Archive& UsArchive() {
    static Archive a;
    return a;
}
Archive& JaArchive() {
    static Archive a;
    return a;
}

// Each scene served from the Japanese disc: its name ("fa1c1081"), its entries, where it is mapped (its subtitle
// units `lead` sectors, then the Japanese file's `count`, from `cursor`).
struct Scene {
    std::string name;
    Entry us, ja;
    bool video = false;
    u32 cursor = 0, lead = 0, count = 0;
};
std::vector<Scene>& Scenes() {
    static std::vector<Scene> scenes;
    return scenes;
}
int g_scene = -1;               // the scene last read (SampleScene), -1 none yet

// The movies whose pictures differ throughout (text cards in the disc's language, section 6): the Japanese one with
// the Japanese dub.
const std::set<std::string> kTextCardMovies = {"st1r6491.pss", "ta1r2101.pss", "ta2r2301.pss"};

// SPANISH's descriptions in the stage scripts (stage/*/*.scx, the VR training's prompts): each text five times in
// the script's string pool - IT, FR, DE, EN, JP - and referenced by the code as "a0 e0 f3" + its offset (u32). An
// English one with a Spanish text (by its hash) is served from its Japanese one's place (never shown by the US
// game), its references turned there; the file served so, in place.
void ScriptsInSpanish(Disc& us, Archive& us_dat) {
    u32 texts = 0, files = 0;
    for (const Entry& e : us_dat.files) {
        if (e.path.size() < 4 || e.path.compare(e.path.size() - 4, 4, ".scx") != 0) continue;
        std::vector<u8> d(std::size_t((e.size + kSectorSize - 1) / kSectorSize) * kSectorSize);
        if (d.empty() || !us.Read(us_dat.base + e.sector, u32(d.size() / kSectorSize), d.data())) continue;
        std::map<u32, std::vector<u32>> refs;           // a string's offset -> where the code names it
        for (std::size_t k = 0; k + 7 <= e.size; ++k)
            if (d[k] == 0xA0 && d[k + 1] == 0xE0 && d[k + 2] == 0xF3) {
                const u32 at = Le32(&d[k + 3]);
                if (at < e.size) refs[at].push_back(u32(k + 3));
            }
        std::vector<u32> strings;
        for (const auto& r : refs) strings.push_back(r.first);
        auto text = [&](u32 a) {
            std::string t;
            for (u32 i = a; i < e.size && d[i]; ++i) t += char(d[i]);
            return t;
        };
        // Each English text with a Spanish one (n: its place among the strings; IT n-3, FR n-2, DE n-1, JP n+1),
        // then the room their Japanese, Italian, German and French texts leave (not shown with SPANISH: the game
        // runs in English) - a string's room up to the next named string.
        auto room = [&](std::size_t n) {
            return (n + 1 < strings.size() ? strings[n + 1] : strings[n] + u32(text(strings[n]).size()) + 1) - strings[n];
        };
        std::vector<std::pair<std::size_t, const char*>> found;
        for (std::size_t n = 3; n + 1 < strings.size(); ++n) {
            const u64 h = Fnv1a(text(strings[n]));
            const SpanishLine* es = FindByHash(Pack().scripts, h);
            if (!es) continue;
            const std::string jt = text(strings[n + 1]);
            if (std::none_of(jt.begin(), jt.end(), [](char c) { return u8(c) >= 0xA1; })) continue;
            found.push_back({n, es->text});
        }
        std::vector<std::pair<u32, u32>> pool;      // (offset, bytes free)
        for (std::ptrdiff_t lang : {1, -3, -1, -2})
            for (const auto& f : found) {
                const std::size_t n = std::size_t(std::ptrdiff_t(f.first) + lang);
                pool.push_back({strings[n], room(n)});
            }
        bool changed = false;
        for (const auto& f : found) {
            const std::size_t len = std::strlen(f.second);
            auto slot = std::find_if(pool.begin(), pool.end(),
                                     [&](const std::pair<u32, u32>& r) { return r.second >= len + 1; });
            if (slot == pool.end()) continue;
            std::memcpy(&d[slot->first], f.second, len + 1);
            for (u32 k : refs[strings[f.first]]) SetLe32(&d[k], slot->first);
            slot->first += u32(len) + 1u;
            slot->second -= u32(len) + 1u;
            changed = true;
            ++texts;
        }
        if (!changed) continue;
        us.MapBytes(us_dat.base + e.sector, std::move(d));
        ++files;
    }
    std::fprintf(stderr, "[dub] Spanish: %u script texts in %u scripts\n", texts, files);
}

// SPANISH or JAPANESE with the English voices: each voiced scene's own subtitle pack served with those lines, in
// place.
void OnEnglishVoices(bool japanese) {
    Disc& us = ps2host::GuestDisc();
    Archive& us_dat = UsArchive();
    if (!us_dat.Open(us)) return;
    u32 scenes = 0;
    for (const Entry& e : us_dat.files) {
        if (e.path.size() < 4 || e.path.compare(e.path.size() - 4, 4, ".pss") != 0) continue;
        std::vector<u8> u(std::min<std::size_t>(64 * kSectorSize, (e.size + kSectorSize - 1) / kSectorSize * kSectorSize));
        if (u.empty() || !us.Read(us_dat.base + e.sector, u32(u.size() / kSectorSize), u.data())) continue;
        const Subtitles subs = ReadSubtitles(u, UsSubtitleChannel);
        if (!subs.end || !subs.ok || subs.end % kSectorSize) continue;
        std::vector<u8> pack = EnglishVoicesPrefix(u, subs, japanese);
        if (pack.empty()) continue;
        us.MapBytes(us_dat.base + e.sector, std::move(pack));
        ++scenes;
    }
    std::fprintf(stderr, "[dub] %s subtitles (from the English) in %u scenes\n", japanese ? "Japanese" : "Spanish",
                 scenes);
    if (!japanese) ScriptsInSpanish(us, us_dat);
}

bool ServeJapanese(Disc& ja, const std::string& path);

void DubOnDiscOpened() {
    WarmOptionsMenu();          // (the OPTIONS words from title.pak, read now and not on the screen's first frame)
    if (ps2host::Setting("dub") != "ja") {
        if (SpanishChosen() || JapaneseChosen()) OnEnglishVoices(JapaneseChosen());
        return;
    }
    // The Japanese voices need the subtitle pack too (the maintainer 2026-11-21: its downloads show the demand for
    // the Japanese dub).
    if (!Pack().loaded) {
        std::fprintf(stderr, "[dub] Japanese chosen, but no subtitle pack (ZOE-subtitles.pak): English\n");
        return;
    }
    std::string path = ps2host::Setting("disc_ja");
    const std::filesystem::path unpacked = ps2platform::ExeDir() / "game" / "disc_ja";
    Disc& ja = JapaneseDisc();
    Disc::File elf;
    // (the launcher's unpacked Japanese disc - the image needed only to unpack it, 2026-11-21)
    if (ja.Open(unpacked.string()) && ja.Find("\\SLPM_650.19;1", &elf)) path = unpacked.string();
    if (path.empty()) {
        std::fprintf(stderr, "[dub] Japanese chosen, but no Japanese disc set (the launcher's second line): English\n");
        return;
    }
    if (path != unpacked.string() && (!ja.Open(path) || !ja.Find("\\SLPM_650.19;1", &elf))) {
        // Moved (2026-11-20 issue 17): an image of it beside the game or in its "game" folder, by its content.
        std::string found;
        const std::filesystem::path here = ps2platform::ExeDir();
        for (const std::filesystem::path& folder : {here, here / "game"}) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                std::string ext = entry.path().extension().string();
                for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                std::error_code is_dir;
                const bool folder_disc = entry.is_directory(is_dir) &&
                                         std::filesystem::exists(entry.path() / "disc.idx", is_dir);
                if (ext != ".iso" && ext != ".chd" && ext != ".bin" && !folder_disc) continue;
                Disc probe;
                Disc::File probe_elf;
                if (probe.Open(entry.path().string()) && probe.Find("\\SLPM_650.19;1", &probe_elf)) {
                    found = entry.path().string();
                    break;
                }
            }
            if (!found.empty()) break;
        }
        if (found.empty() || !ja.Open(found) || !ja.Find("\\SLPM_650.19;1", &elf)) {
            std::fprintf(stderr, "[dub] the Japanese disc (SLPM-65019) is not at %s, nor beside the game or in its "
                         "\"game\" folder: English voices\n", path.c_str());
            return;
        }
        std::fprintf(stderr, "[dub] the Japanese disc found at %s (was %s)\n", found.c_str(), path.c_str());
        ps2host::SetSetting("disc_ja", found);
        path = found;
    }
    ServeJapanese(ja, path);
}

// The Japanese voices served from `ja` (an image or the unpacked folder): each voice scene and speaking movie of its
// ZOE.DAT mapped onto the US disc. False: its ZOE.DAT not read.
bool ServeJapanese(Disc& ja, const std::string& path) {
    Disc& us = ps2host::GuestDisc();
    Archive& us_dat = UsArchive();
    Archive& ja_dat = JaArchive();
    if (!us_dat.Open(us) || !ja_dat.Open(ja)) {
        std::fprintf(stderr, "[dub] ZOE.DAT's table not read: English\n");
        return false;
    }
    std::map<std::string, const Entry*> ja_files;
    for (const Entry& e : ja_dat.files) ja_files[e.path] = &e;

    u32 cursor = us.Sectors();  // past the US image's end
    std::set<u32> changed;
    u32 voices = 0, movies = 0, retimed = 0;
    for (const Entry& e : us_dat.files) {
        if (e.path.size() < 4 || e.path.compare(e.path.size() - 4, 4, ".pss") != 0) continue;
        const auto found = ja_files.find(e.path);
        if (found == ja_files.end()) continue;
        const std::string name = e.path.substr(e.path.rfind('/') + 1);
        const bool video = HasVideo(us_dat, e);
        const Entry& j = *found->second;
        const u32 count = (j.size + kSectorSize - 1) / kSectorSize;
        if (!count) continue;
        // A movie that speaks (subtitles on the US disc) or a text card: the Japanese one; a silent movie stays.
        std::vector<u8> prefix = SubtitlePrefix(us_dat, e, ja_dat, j, video, ModeOf(name));
        if (video && prefix.empty() && !kTextCardMovies.count(name)) continue;
        const u32 lead = u32(prefix.size() / kSectorSize);
        if (lead) {
            us.MapBytes(cursor, std::move(prefix));
            ++retimed;
        }
        us.MapSectors(cursor + lead, count, &ja, ja_dat.base + j.sector);
        u8* raw = us_dat.tables[e.table].data() + std::size_t(e.index) * 32;
        SetLe32(raw + 16, cursor - us_dat.base);
        SetLe32(raw + 20, lead * kSectorSize + j.size);
        changed.insert(e.table);
        Scenes().push_back(Scene{name.substr(0, name.rfind('.')), e, j, video, cursor, lead, count});
        cursor += lead + count;
        (video ? movies : voices)++;
    }
    // A margin after the last scene: the game streams a scene in whole chunks and reads past a file's end (on the
    // disc, into what follows it); past the mapped end Disc::Read fails - the last scene mapped is the VR
    // training's (vr1a8181), and its end was the "disc read error" after the training with the Japanese voices
    // (2026-11-20 issue 7). 1 MiB of zeros.
    if (voices + movies) {
        us.MapBytes(cursor, std::vector<u8>(std::size_t(512) * kSectorSize, 0));
        cursor += 512;
    }
    for (u32 table : changed) us.MapBytes(us_dat.base + table, us_dat.tables[table]);
    if (SpanishChosen()) ScriptsInSpanish(us, us_dat);
    std::fprintf(stderr, "[dub] Japanese: %u voice scenes and %u movies from %s, %u with their subtitles moved onto "
                 "the Japanese voice (%u tables rewritten, the disc now %u sectors)\n", voices, movies, path.c_str(),
                 retimed, u32(changed.size()), us.Sectors());
    return true;
}

// The launcher's step (`zoe_us.exe --unpack-extra <Japanese image> <folder> --disc <US disc>`, the maintainer
// 2026-11-21: of the Japanese disc only what is needed): the voices served as at a start, every sector read or
// mapped from the Japanese disc meanwhile recorded - its file system's tables, SLPM_650.19 and SYSTEM.CNF, ZOE.DAT's
// tables, each voice scene and speaking movie - and only those unpacked.
bool UnpackJapanese(const std::string& image, const std::string& folder) {
    Disc& ja = JapaneseDisc();
    Disc::File elf;
    if (!ja.Open(image) || !ja.Find("\\SLPM_650.19;1", &elf)) {
        std::fprintf(stderr, "[unpack] %s is no image of the Japanese disc (SLPM-65019)\n", image.c_str());
        return false;
    }
    ja.Record(true);
    // (the file system's tables, the boot file and SYSTEM.CNF read too: the folder is a disc of its own)
    std::vector<u8> bytes;
    ja.ReadFile("\\SLPM_650.19;1", &bytes);
    ja.ReadFile("SYSTEM.CNF", &bytes);
    const bool served = ServeJapanese(ja, image);
    ja.Record(false);
    if (!served) return false;
    std::string progress = folder + ".progress";
    const bool ok = ja.Unpack(folder, &ja.Recorded(), ps2host::WriteUnpackProgress, &progress);
    std::fprintf(stderr, "[unpack] the Japanese disc: %zu ranges read, %s\n", ja.Recorded().size(), ok ? "unpacked" : "FAILED");
    return ok;
}

}  // namespace

void RegisterDub() {
    ps2host::SetDiscOpened(DubOnDiscOpened);
    ps2host::SetExtraDiscUnpacker(UnpackJapanese);
}

// SPANISH's descriptions in the executable (weapons and items, passcodes, mission goals, tips - tools/m10/
// desc_tables.py: per entry JP, EN, FR, DE, IT groups of `group` line pointers): an entry whose English lines are as
// expected (their hash) gets its Spanish lines - written where the Japanese, Italian, German and French ones were
// (not shown while the game runs in English; SPANISH is chosen at the start) - and its English pointers on them.
// Called as the program sets its language (overrides.cpp LanguageAtStart), the tables static data.
void DescriptionsInSpanish() {
    if (!SpanishChosen()) return;
    constexpr u32 kEmpty = 0x004B6BF0u;             // the tables' "" string
    auto& m = ps2host::GuestMemory();
    auto read = [&](u32 a) {
        std::string t;
        for (u32 i = 0; i < 512u; ++i) {
            const char c = char(m.Read(a + i, 1));
            if (!c) break;
            t += c;
        }
        return t;
    };
    auto pointer = [&](const SpanishDescription& d, int lang, u32 i) {   // lang -1 JP, 0 EN, 1 FR, 2 DE, 3 IT
        return u32(m.Read(u32(int(d.at) + 4 * (lang * int(d.group) + int(i))), 4));
    };
    std::vector<std::pair<u32, u32>> pool;          // (address, bytes free)
    std::set<u32> seen;
    for (int lang : {-1, 3, 2, 1})
        for (const SpanishDescription& d : Pack().descriptions)
            for (u32 i = 0; i < d.group; ++i) {
                const u32 p = pointer(d, lang, i);
                if (p == kEmpty || p < 0x00100000u || p >= 0x02000000u || !seen.insert(p).second) continue;
                pool.push_back({p, u32(read(p).size()) + 1u});
            }
    u32 done = 0, wrong = 0, full = 0;
    for (const SpanishDescription& d : Pack().descriptions) {
        std::vector<std::string> en;
        for (u32 i = 0; i < d.group; ++i) {
            const u32 p = pointer(d, 0, i);
            en.push_back(p == kEmpty ? std::string() : read(p));
        }
        while (!en.empty() && en.back().empty()) en.pop_back();
        std::string joined;
        for (std::size_t i = 0; i < en.size(); ++i) joined += (i ? "\n" : "") + en[i];
        if (Fnv1a(joined) != d.hash) {
            ++wrong;
            continue;
        }
        std::vector<std::string> es;
        for (const char* a = d.text;;) {
            const char* b = std::strchr(a, '\n');
            es.push_back(b ? std::string(a, b) : std::string(a));
            if (!b) break;
            a = b + 1;
        }
        std::vector<u32> places;
        for (const std::string& line : es) {
            auto slot = std::find_if(pool.begin(), pool.end(),
                                     [&](const std::pair<u32, u32>& r) { return r.second >= line.size() + 1; });
            if (slot == pool.end()) break;
            for (std::size_t i = 0; i < line.size(); ++i) m.Write(slot->first + u32(i), u8(line[i]), 1);
            m.Write(slot->first + u32(line.size()), 0u, 1);
            places.push_back(slot->first);
            slot->first += u32(line.size()) + 1u;
            slot->second -= u32(line.size()) + 1u;
        }
        if (places.size() != es.size() || es.size() > d.group) {
            ++full;
            continue;
        }
        for (u32 i = 0; i < d.group; ++i)
            m.Write(u32(int(d.at) + 4 * int(i)), i < places.size() ? places[i] : kEmpty, 4);
        ++done;
    }
    std::fprintf(stderr, "[dub] Spanish descriptions: %u of %u (%u not as expected, %u without room)\n", done,
                 u32(Pack().descriptions.size()), wrong, full);
}

// The subtitle pack (for the language list and the Japanese font: overrides.cpp).
bool DubSubtitlePack() { return Pack().loaded; }
const std::vector<unsigned char>& DubSubtitleFont() { return Pack().font; }

// The Japanese line a subtitle's text stands for ("#J" + its hash), UTF-8; "" when it is no such marker.
std::string DubJapaneseLine(const std::string& text) {
    if (text.size() < 18 || text.compare(0, 2, "#J") != 0) return {};
    const u64 h = std::strtoull(text.c_str() + 2, nullptr, 16);
    const auto it = JapaneseTexts().find(h);
    return it == JapaneseTexts().end() ? std::string() : it->second;
}

// SUBTITLE TIMING (the F9 debug panel, internal; the maintainer 2026-11-21): the scene the game read last (sampled
// each frame - FrameEnd), its way (auto / jp / us: "dub_subs_<scene>"), stepped there and served at once - its
// subtitle units made again and mapped over the old ones (the game reads them when the scene next plays).
void DubSampleScene() {
    const u32 lsn = ps2host::GuestDisc().LastReadLsn();
    const std::vector<Scene>& scenes = Scenes();
    for (int i = 0; i < int(scenes.size()); ++i) {
        const Scene& s = scenes[std::size_t(i)];
        if (lsn >= s.cursor && lsn < s.cursor + s.lead + s.count) {
            g_scene = i;
            return;
        }
    }
}

std::string DubSceneLine() {
    if (g_scene < 0) return Scenes().empty() ? "no Japanese voices" : "none played yet";
    const Scene& s = Scenes()[std::size_t(g_scene)];
    const SubsMode mode = ModeOf(s.name + ".pss");
    return s.name + (s.video ? " (movie) " : " ") + (mode == SubsMode::kJp ? "JP" : mode == SubsMode::kUs ? "US" : "AUTO");
}

void DubSceneStep(int step) {
    if (g_scene < 0) return;
    const Scene& s = Scenes()[std::size_t(g_scene)];
    static const char* const kModes[3] = {"auto", "jp", "us"};
    const SubsMode now = ModeOf(s.name + ".pss");
    int at = now == SubsMode::kJp ? 1 : now == SubsMode::kUs ? 2 : 0;
    at = (at + (step > 0 ? 1 : 2)) % 3;
    ps2host::SetSetting("dub_subs_" + s.name, kModes[at]);
    std::vector<u8> prefix = SubtitlePrefix(UsArchive(), s.us, JaArchive(), s.ja, s.video, ModeOf(s.name + ".pss"));
    if (prefix.size() / kSectorSize == s.lead && s.lead) {
        ps2host::GuestDisc().MapBytes(s.cursor, std::move(prefix));
        std::fprintf(stderr, "[dub] %s: subtitle timing %s (served from its next playing)\n", s.name.c_str(), kModes[at]);
    }
}

// A file of the running disc's ZOE.DAT ("stage/title/title.pak"), whole; false when it is not there.
bool ReadZoeDatFile(const std::string& path, std::vector<unsigned char>* out) {
    Archive dat;
    if (!dat.Open(ps2host::GuestDisc())) return false;
    for (const Entry& e : dat.files) {
        if (e.path != path) continue;
        const u32 count = (e.size + kSectorSize - 1) / kSectorSize;
        out->assign(std::size_t(count) * kSectorSize, 0);
        if (!count || !dat.disc->Read(dat.base + e.sector, count, out->data())) return false;
        out->resize(e.size);
        return true;
    }
    return false;
}
