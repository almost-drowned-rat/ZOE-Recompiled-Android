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
#include "ps2host/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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

// One language's cues moved onto the Japanese voice: with as many Japanese cues, the Japanese times one for one
// (both in time order); else the line's span stretched onto the Japanese cues' span; without Japanese cues, scaled
// by the two files' lengths (the voice data's rate is the same on both discs).
void Retime(std::vector<u8>& d, const Channel& channel, std::vector<Cue> ja, u32 us_size, u32 ja_size) {
    if (channel.cues.empty()) return;
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
std::vector<u8> SubtitlePrefix(Archive& us_dat, const Entry& us, Archive& ja_dat, const Entry& ja) {
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
    for (const Channel& channel : us_subs.channels) Retime(prefix, channel, ja_cues, us.size, ja.size);
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

// The movies whose pictures differ throughout (text cards in the disc's language, section 6): the Japanese one with
// the Japanese dub.
const std::set<std::string> kTextCardMovies = {"st1r6491.pss", "ta1r2101.pss", "ta2r2301.pss"};

void DubOnDiscOpened() {
    WarmOptionsMenu();          // (the OPTIONS words from title.pak, read now and not on the screen's first frame)
    if (ps2host::Setting("dub") != "ja") return;
    const std::string path = ps2host::Setting("disc_ja");
    if (path.empty()) {
        std::fprintf(stderr, "[dub] Japanese chosen, but no Japanese disc set (the launcher's second line): English\n");
        return;
    }
    Disc& ja = JapaneseDisc();
    Disc::File elf;
    if (!ja.Open(path) || !ja.Find("\\SLPM_650.19;1", &elf)) {
        std::fprintf(stderr, "[dub] %s is not the Japanese disc (SLPM-65019): English\n", path.c_str());
        return;
    }
    Disc& us = ps2host::GuestDisc();
    Archive us_dat, ja_dat;
    if (!us_dat.Open(us) || !ja_dat.Open(ja)) {
        std::fprintf(stderr, "[dub] ZOE.DAT's table not read: English\n");
        return;
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
        std::vector<u8> prefix = SubtitlePrefix(us_dat, e, ja_dat, j);
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
        cursor += lead + count;
        (video ? movies : voices)++;
    }
    for (u32 table : changed) us.MapBytes(us_dat.base + table, us_dat.tables[table]);
    std::fprintf(stderr, "[dub] Japanese: %u voice scenes and %u movies from %s, %u with their subtitles moved onto "
                 "the Japanese voice (%u tables rewritten, the disc now %u sectors)\n", voices, movies, path.c_str(),
                 retimed, u32(changed.size()), us.Sectors());
}

}  // namespace

void RegisterDub() { ps2host::SetDiscOpened(DubOnDiscOpened); }

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
