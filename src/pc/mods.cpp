/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pop-in characters for melee-pc (MeleeForge).
 *
 * A custom character is a .ssb package (a zip) in mods/, mods/characters/ or mods/skins/, next to the executable
 * (or under $MELEE_MODS_DIR):
 *   Leon.ssb
 *     skin.json   {"type": "character", "name": "Leon", "fighter": "Ca", "slot": 0, "file": "skin.dat", ...}
 *     skin.dat    a complete costume archive built on that fighter (its skeleton, so its moves fit)
 *     icon.png    optional
 * or the same files unpacked in a folder.
 *
 * It plays with the base fighter's moves and sits beside the real roster: the CHARACTERS icon on character select
 * lists them, each with P1-P4 buttons. A player given a custom character picks its base fighter on the grid, and
 * when that player's fighter is created (ft/fighter.c) it gets the custom model instead of the costume's, loaded
 * for that player alone. Everything else (costume number, stock icons, menus) stays the base fighter's, so the
 * game's tables are never indexed past their ends. P1 can be Falcon while P2 is Leon on Falcon's moves.
 *
 * The model file reaches the game through aurora's DVD overlay as "mfskins/<hash>.dat" (the hash changes with the
 * file, so a rebuilt character never hits a stale cache entry) and is loaded with the game's own archive loader
 * into the same per-scene heap as the real costumes. Nothing applies during online play: both peers must load
 * the same files. */

#include "mods.h"
#include "launcher.h"
#include "net.h"
#include "pc.h"

#include <dolphin/pad.h>

#include "../../extern/miniz/mf_miniz.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <aurora/dvd.h>
#include <aurora/rmlui.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern "C" {
/* src/melee/lb/lbarchive.c: load an archive into the scene heap and look up its roots ("name", &out pairs, then
 * NULL). The real costumes are loaded the same way (ftData_80085820). */
bool lbArchive_80017040(void** dst, const char* filename, void* symbols, ...);
}

namespace {

/* Fighter kinds in FighterKind order: two-letter code, display name. */
struct Kind {
    const char* code;
    const char* name;
};
const Kind kKinds[] = {
    {"Mr", "Mario"},          {"Fx", "Fox"},         {"Ca", "Captain Falcon"}, {"Dk", "Donkey Kong"},
    {"Kb", "Kirby"},          {"Kp", "Bowser"},      {"Lk", "Link"},           {"Sk", "Sheik"},
    {"Ns", "Ness"},           {"Pe", "Peach"},       {"Pp", "Popo"},           {"Nn", "Nana"},
    {"Pk", "Pikachu"},        {"Ss", "Samus"},       {"Ys", "Yoshi"},          {"Pr", "Jigglypuff"},
    {"Mt", "Mewtwo"},         {"Lg", "Luigi"},       {"Ms", "Marth"},          {"Zd", "Zelda"},
    {"Cl", "Young Link"},     {"Dr", "Dr. Mario"},   {"Fc", "Falco"},          {"Pc", "Pichu"},
    {"Gw", "Mr. Game & Watch"}, {"Gn", "Ganondorf"}, {"Fe", "Roy"},            {"Mh", "Master Hand"},
    {"Ch", "Crazy Hand"},     {"Bo", "Male Wireframe"}, {"Gl", "Female Wireframe"}, {"Gk", "Giga Bowser"},
    {"Sb", "Sandbag"},
};
constexpr int kKindCount = (int)(sizeof(kKinds) / sizeof(kKinds[0]));
constexpr int kPorts = 4;      /* players a character can be given to */
constexpr int kPlayerMax = 8;  /* player slots the game can create fighters for */

struct Character {
    std::string folder;   /* folder or .ssb file name: the key in characters.txt */
    std::string package;  /* host path of the .ssb, empty for a folder */
    std::string entry;    /* the model's name inside the .ssb */
    std::string datPath;  /* host path of the model (folders) */
    std::string name, author;
    int kind = -1;        /* base fighter */
    int slot = 0;         /* the base costume the model was built from */
    std::string joint, matanim; /* the model's root names (matanim may be empty) */
    std::string overlay;  /* "/mfskins/xxxxxxxx.dat" */
    std::string gameName; /* overlay without the leading slash: what the game loads */
    uint64_t size = 0;
};

std::vector<std::unique_ptr<Character>> g_chars;
std::vector<std::string> g_problems;
int g_port[kPorts] = {-1, -1, -1, -1}; /* character index per player, -1 = none */
bool g_css = false;
bool g_menu_open = false;
fs::path g_dir; /* mods/ */

/* per player: the model loaded for the current scene */
struct Loaded {
    bool valid = false;
    int ch = -1, kind = -1;
    void* base = nullptr; /* the game's own costume joint when this was loaded */
    void* archive = nullptr;
    void* joint = nullptr;
    void* matanim = nullptr;
};
Loaded g_loaded[kPlayerMax];

std::string U8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string Lower(std::string s) {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Flat JSON objects only: "key": "string" | number | true/false. Enough for skin.json. */
std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    auto skipWs = [&] {
        while (i < text.size() && std::isspace((unsigned char)text[i]))
            i++;
    };
    auto readString = [&](std::string& s) {
        if (i >= text.size() || text[i] != '"')
            return false;
        i++;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                char e = text[++i];
                s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                s += text[i];
            }
            i++;
        }
        i++;
        return true;
    };
    while (i < text.size()) {
        size_t q = text.find('"', i);
        if (q == std::string::npos)
            break;
        i = q;
        std::string key;
        if (!readString(key))
            break;
        skipWs();
        if (i >= text.size() || text[i] != ':')
            continue;
        i++;
        skipWs();
        std::string val;
        if (i < text.size() && text[i] == '"') {
            readString(val);
        } else {
            while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != '\n')
                val += text[i++];
            while (!val.empty() && std::isspace((unsigned char)val.back()))
                val.pop_back();
            if (!val.empty() && (val[0] == '{' || val[0] == '['))
                continue; /* nested: not ours */
        }
        out[key] = val;
    }
    return out;
}

int KindFromCode(const std::string& s) {
    std::string l = Lower(s);
    if (l.size() == 4 && l.rfind("pl", 0) == 0)
        l = l.substr(2);
    for (int k = 0; k < kKindCount; k++) {
        if (Lower(kKinds[k].code) == l || Lower(kKinds[k].name) == l || std::to_string(k) == l)
            return k;
    }
    return -1;
}

/* The model's root names, read from its HSD archive header (so a hand-made package can't name the wrong ones). */
bool ArchiveRoots(const std::vector<uint8_t>& f, std::string& joint, std::string& matanim) {
    auto be32 = [&](size_t o) -> uint32_t {
        return o + 4 <= f.size() ? (uint32_t)f[o] << 24 | (uint32_t)f[o + 1] << 16 | (uint32_t)f[o + 2] << 8 | f[o + 3]
                                 : 0;
    };
    if (f.size() < 0x20)
        return false;
    uint64_t data = be32(4), nreloc = be32(8), npub = be32(12), next = be32(16);
    uint64_t pub = 0x20 + data + 4 * nreloc;
    uint64_t strs = pub + 8 * (npub + next);
    if (strs > f.size() || npub > 4096)
        return false;
    joint.clear();
    matanim.clear();
    for (uint64_t i = 0; i < npub; i++) {
        uint64_t so = strs + be32(pub + 8 * i + 4);
        if (so >= f.size())
            continue;
        std::string n((const char*)f.data() + so, strnlen((const char*)f.data() + so, f.size() - so));
        const std::string suf = "_Share_joint", msuf = "_Share_matanim_joint";
        if (n.size() > msuf.size() && n.compare(n.size() - msuf.size(), msuf.size(), msuf) == 0)
            matanim = n;
        else if (n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
            joint = n;
    }
    return !joint.empty();
}

fs::path ModsDir() {
    const char* env = std::getenv("MELEE_MODS_DIR");
    if (env && *env)
        return fs::path(env);
    const char* base = SDL_GetBasePath();
    return fs::path(base ? base : ".") / "mods";
}

/* ---- DVD overlay ------------------------------------------------------------------------------------------- */

FILE* OpenFile(const std::string& path) {
#ifdef _WIN32
    std::wstring w = fs::path(std::u8string(path.begin(), path.end())).wstring();
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

/* An open overlay file: a folder skin streams from disk, a packaged one is unpacked into memory on open. */
struct Handle {
    FILE* f = nullptr;
    std::vector<uint8_t> mem;
    size_t pos = 0;
};

bool ReadPackageEntry(const std::string& package, const std::string& entry, std::vector<uint8_t>& out) {
    FILE* f = OpenFile(package);
    if (!f)
        return false;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    bool ok = false;
    if (mz_zip_reader_init_cfile(&zip, f, 0, 0)) {
        size_t size = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
        if (p) {
            out.assign((uint8_t*)p, (uint8_t*)p + size);
            mz_free(p);
            ok = true;
        }
        mz_zip_reader_end(&zip);
    }
    std::fclose(f);
    return ok;
}

void* OverlayOpen(void* userdata) {
    const Character* skin = static_cast<const Character*>(userdata);
    auto* h = new Handle();
    if (skin->package.empty()) {
        h->f = OpenFile(skin->datPath);
        if (!h->f) {
            delete h;
            return nullptr;
        }
    } else if (!ReadPackageEntry(skin->package, skin->entry, h->mem)) {
        pc_log_line("mods: couldn't unpack %s from %s", skin->entry.c_str(), skin->package.c_str());
        delete h;
        return nullptr;
    }
    return h;
}
void OverlayClose(void* p) {
    auto* h = static_cast<Handle*>(p);
    if (!h)
        return;
    if (h->f)
        std::fclose(h->f);
    delete h;
}
int64_t OverlayRead(void* p, uint8_t* buf, size_t len) {
    auto* h = static_cast<Handle*>(p);
    if (!h)
        return -1;
    if (h->f)
        return (int64_t)std::fread(buf, 1, len, h->f);
    size_t n = h->pos < h->mem.size() ? std::min(len, h->mem.size() - h->pos) : 0;
    if (n)
        std::memcpy(buf, h->mem.data() + h->pos, n);
    h->pos += n;
    return (int64_t)n;
}
int64_t OverlaySeek(void* p, int64_t off, int32_t whence) {
    auto* h = static_cast<Handle*>(p);
    if (!h)
        return -1;
    if (h->f) {
#ifdef _WIN32
        if (_fseeki64(h->f, off, whence) != 0)
            return -1;
        return _ftelli64(h->f);
#else
        if (fseeko(h->f, (off_t)off, whence) != 0)
            return -1;
        return (int64_t)ftello(h->f);
#endif
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)h->pos : (int64_t)h->mem.size();
    int64_t to = base + off;
    if (to < 0)
        return -1;
    h->pos = (size_t)to;
    return to;
}

/* stable per file version: folder name + size + time */
std::string OverlayName(const std::string& folder, uint64_t size, int64_t time) {
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++)
            h = (h ^ b[i]) * 16777619u;
    };
    mix(folder.data(), folder.size());
    mix(&size, sizeof(size));
    mix(&time, sizeof(time));
    char buf[40];
    std::snprintf(buf, sizeof(buf), "/mfskins/%08x.dat", h);
    return buf;
}

void RegisterOverlay() {
    static bool callbacksSet = false;
    if (!callbacksSet) {
        static const AuroraOverlayCallbacks cb = {OverlayOpen, OverlayClose, OverlayRead, OverlaySeek};
        aurora_dvd_overlay_callbacks(&cb);
        callbacksSet = true;
    }
    std::vector<AuroraOverlayFile> files;
    for (auto& c : g_chars)
        files.push_back({c->overlay.c_str(), c.get(), (size_t)c->size});
    std::vector<s32> nums(files.size(), -1);
    aurora_dvd_overlay_files(files.empty() ? nullptr : files.data(), files.size(),
        nums.empty() ? nullptr : nums.data());
    for (size_t i = 0; i < nums.size(); i++) {
        if (nums[i] < 0) {
            g_problems.push_back(g_chars[i]->folder + ": the game refused the file");
            g_chars[i]->kind = -1;
        }
    }
}

/* ---- player assignments ------------------------------------------------------------------------------------ */

void SaveAssignments() {
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    std::ofstream f(g_dir / "characters.txt");
    for (int p = 0; p < kPorts; p++)
        if (g_port[p] >= 0 && g_port[p] < (int)g_chars.size())
            f << "P" << (p + 1) << "=" << g_chars[g_port[p]]->folder << "\n";
}

void LoadAssignments() {
    for (int& p : g_port)
        p = -1;
    std::ifstream f(g_dir / "characters.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.size() < 4 || line[0] != 'P' || line[2] != '=')
            continue;
        int p = line[1] - '1';
        if (p < 0 || p >= kPorts)
            continue;
        std::string key = line.substr(3);
        for (size_t i = 0; i < g_chars.size(); i++)
            if (g_chars[i]->folder == key && g_chars[i]->kind >= 0)
                g_port[p] = (int)i;
    }
}

void InvalidateLoaded() {
    for (auto& l : g_loaded)
        l = Loaded();
}

/* ---- finding characters ------------------------------------------------------------------------------------- */

void Scan() {
    g_chars.clear();
    g_problems.clear();
    InvalidateLoaded();
    g_dir = ModsDir();
    std::error_code ec;

    /* skin.json + the model's bytes -> a character, or a problem line */
    auto add = [&](std::unique_ptr<Character> c, const std::string& json, const std::vector<uint8_t>& dat,
                   uint64_t containerSize, int64_t containerTime) {
        auto j = ParseFlatJson(json);
        std::string type = j.count("type") ? j["type"] : "";
        if (!type.empty() && type != "character" && type != "skin")
            return; /* a stage or audio package: not this loader's */
        c->name = j.count("name") && !j["name"].empty() ? j["name"] : c->folder;
        c->author = j["author"];
        c->kind = KindFromCode(j.count("base") && !j["base"].empty() ? j["base"] : j["fighter"]);
        c->slot = j.count("slot") ? std::atoi(j["slot"].c_str()) : 0;
        if (c->kind < 0) {
            g_problems.push_back(c->folder + ": unknown base fighter \"" + j["fighter"] + "\"");
            return;
        }
        if (dat.empty()) {
            g_problems.push_back(c->folder + ": the model file is missing");
            return;
        }
        if (!ArchiveRoots(dat, c->joint, c->matanim)) {
            g_problems.push_back(c->folder + ": the model isn't a fighter costume file");
            return;
        }
        c->size = dat.size();
        c->overlay = OverlayName(c->folder, containerSize, containerTime);
        c->gameName = c->overlay.substr(1);
        g_chars.push_back(std::move(c));
    };

    /* .ssb packages in mods/, mods/characters/, mods/skins/; unpacked folders in the last two */
    std::vector<fs::path> packages, dirs;
    for (const fs::path& where : {g_dir, g_dir / "characters", g_dir / "skins"}) {
        if (!fs::is_directory(where, ec))
            continue;
        for (auto& e : fs::directory_iterator(where, ec)) {
            std::string ext = Lower(U8(e.path().extension()));
            if (ext == ".ssb" && e.is_regular_file(ec))
                packages.push_back(e.path());
            else if (where != g_dir && e.is_directory(ec))
                dirs.push_back(e.path());
        }
    }
    std::sort(packages.begin(), packages.end(), [](auto& a, auto& b) { return a.filename() < b.filename(); });
    std::sort(dirs.begin(), dirs.end(), [](auto& a, auto& b) { return a.filename() < b.filename(); });
    std::set<std::string> seen;
    for (auto& pkg : packages) {
        std::string key = U8(pkg.filename());
        if (!seen.insert(Lower(key)).second) {
            g_problems.push_back(key + ": there's more than one copy (using the first)");
            continue;
        }
        FILE* f = OpenFile(U8(pkg));
        if (!f) {
            g_problems.push_back(key + ": can't open");
            continue;
        }
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_cfile(&zip, f, 0, 0)) {
            std::fclose(f);
            g_problems.push_back(key + ": not a valid .ssb (zip) file");
            continue;
        }
        size_t n = 0;
        char* json = (char*)mz_zip_reader_extract_file_to_heap(&zip, "skin.json", &n, 0);
        std::string text = json ? std::string(json, n) : std::string();
        if (json)
            mz_free(json);
        auto j = ParseFlatJson(text);
        std::string file = j.count("file") && !j["file"].empty() ? j["file"] : "skin.dat";
        std::vector<uint8_t> dat;
        void* p = json ? mz_zip_reader_extract_file_to_heap(&zip, file.c_str(), &n, 0) : nullptr;
        if (p) {
            dat.assign((uint8_t*)p, (uint8_t*)p + n);
            mz_free(p);
        }
        mz_zip_reader_end(&zip);
        std::fclose(f);
        if (!json) {
            g_problems.push_back(key + ": no skin.json inside");
            continue;
        }
        auto c = std::make_unique<Character>();
        c->folder = key;
        c->package = U8(pkg);
        c->entry = file;
        add(std::move(c), text, dat, (uint64_t)fs::file_size(pkg, ec),
            (int64_t)fs::last_write_time(pkg, ec).time_since_epoch().count());
    }
    for (auto& d : dirs) {
        std::string key = U8(d.filename());
        std::ifstream jf(d / "skin.json");
        if (!jf)
            continue;
        std::stringstream ss;
        ss << jf.rdbuf();
        auto j = ParseFlatJson(ss.str());
        std::string file = j.count("file") && !j["file"].empty() ? j["file"] : "skin.dat";
        fs::path datPath = d / file;
        std::ifstream df(datPath, std::ios::binary);
        std::vector<uint8_t> dat((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
        auto c = std::make_unique<Character>();
        c->folder = key;
        c->datPath = U8(datPath);
        add(std::move(c), ss.str(), dat, dat.size(),
            (int64_t)fs::last_write_time(datPath, ec).time_since_epoch().count());
    }
    RegisterOverlay();
    LoadAssignments();
    pc_log_line("mods: %u character(s) in %s, %u problem(s)", (unsigned)g_chars.size(), U8(g_dir).c_str(),
        (unsigned)g_problems.size());
}

/* ---- the icon and dropdown (RmlUi) -------------------------------------------------------------------------- */

const char* kDocument = R"RML(<rml><head><title>Characters</title><style>
body { font-family: "Liberation Sans"; font-size: 15dp; color: #f2f2f8; position: absolute; top: 10dp; right: 12dp;
       width: 360dp; z-index: 90; }
#icon { display: block; margin-left: auto; width: 150dp; height: 32dp; line-height: 32dp; text-align: center;
        background-color: #1c2240e6; border: 2dp #ffd84a; border-radius: 6dp; font-weight: bold; color: #ffd84a;
        cursor: pointer; }
#icon:hover { background-color: #2a3360f0; }
#panel { display: none; margin-top: 6dp; background-color: #0b0f22f0; border: 1dp #4a5590; border-radius: 6dp;
         padding: 8dp; max-height: 480dp; overflow-y: auto; }
#panel.open { display: block; }
.hint { color: #aab0d0; font-size: 12dp; margin-bottom: 6dp; }
.char { display: block; padding: 6dp 8dp; border-radius: 4dp; margin-bottom: 4dp; background-color: #151b38; }
.char .sub { font-size: 12dp; color: #aab0d0; }
.ports { display: block; margin-top: 4dp; }
.port { display: inline-block; width: 40dp; height: 22dp; line-height: 22dp; text-align: center; margin-right: 4dp;
        border-radius: 4dp; background-color: #2a3150; color: #c8cce8; font-size: 13dp; cursor: pointer; }
.port:hover { background-color: #3a4370; }
.port.on { background-color: #ffd84a; color: #1a1a2a; font-weight: bold; }
.who { color: #ffd84a; font-size: 13dp; margin-top: 6dp; }
.empty { color: #aab0d0; font-size: 13dp; }
.problem { color: #ff9a8a; font-size: 12dp; }
.foot { margin-top: 8dp; font-size: 12dp; color: #8890b8; }
#reload { color: #9ad0ff; cursor: pointer; }
</style></head><body><div id="icon">CHARACTERS &#9662;</div><div id="panel"></div></body></rml>)RML";

std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<')
            o += "&lt;";
        else if (c == '>')
            o += "&gt;";
        else if (c == '&')
            o += "&amp;";
        else if (c == '"')
            o += "&quot;";
        else
            o += c;
    }
    return o;
}

struct Menu : Rml::EventListener {
    Rml::ElementDocument* doc = nullptr;

    void Build() {
        if (!doc)
            return;
        Rml::Element* panel = doc->GetElementById("panel");
        std::string r;
        if (pc_net_active())
            r += "<div class='problem'>Online play: custom characters are off for this session.</div>";
        if (g_chars.empty()) {
            r += "<div class='empty'>No characters yet. Build one in MeleeForge, or drop a .ssb into<br/>" +
                 Escape(U8(g_dir)) + "</div>";
        } else {
            r += "<div class='hint'>Give a character to a player, then that player picks the fighter it plays "
                 "as.</div>";
        }
        for (size_t i = 0; i < g_chars.size(); i++) {
            Character& c = *g_chars[i];
            if (c.kind < 0)
                continue;
            r += "<div class='char'>" + Escape(c.name) + "<br/><span class='sub'>plays as " +
                 Escape(kKinds[c.kind].name) + (c.author.empty() ? "" : " &middot; by " + Escape(c.author)) +
                 "</span><div class='ports'>";
            for (int p = 0; p < kPorts; p++) {
                r += "<span class='port" + std::string(g_port[p] == (int)i ? " on" : "") + "' id='c-" +
                     std::to_string(i) + "-" + std::to_string(p) + "'>P" + std::to_string(p + 1) + "</span>";
            }
            r += "</div></div>";
        }
        for (int p = 0; p < kPorts; p++) {
            if (g_port[p] >= 0 && g_port[p] < (int)g_chars.size()) {
                const Character& c = *g_chars[g_port[p]];
                r += "<div class='who'>P" + std::to_string(p + 1) + ": " + Escape(c.name) + " &mdash; pick " +
                     Escape(kKinds[c.kind].name) + "</div>";
            }
        }
        for (auto& pr : g_problems)
            r += "<div class='problem'>" + Escape(pr) + "</div>";
        r += "<div class='foot'><span id='reload'>Reload characters</span> &middot; F2</div>";
        panel->SetInnerRML(r);
    }

    void SetOpen(bool open) {
        if (open != g_menu_open)
            PADBlockInput(open); /* the pad shouldn't move the CSS cursor while the dropdown is open */
        g_menu_open = open;
        if (!doc)
            return;
        doc->GetElementById("panel")->SetClass("open", open);
        if (open)
            Build();
    }

    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* t = event.GetTargetElement();
        while (t && t != doc) {
            const Rml::String& id = t->GetId();
            if (id == "icon") {
                SetOpen(!g_menu_open);
                return;
            }
            if (id == "reload") {
                Scan();
                Build();
                return;
            }
            if (id.rfind("c-", 0) == 0) {
                int ch = -1, p = -1;
                if (std::sscanf(id.c_str(), "c-%d-%d", &ch, &p) == 2 && p >= 0 && p < kPorts && ch >= 0 &&
                    ch < (int)g_chars.size()) {
                    g_port[p] = g_port[p] == ch ? -1 : ch;
                    SaveAssignments();
                    Build();
                }
                return;
            }
            t = t->GetParentNode();
        }
    }
};
Menu g_menu;

} // namespace

/* ---- entry points ------------------------------------------------------------------------------------------- */

extern "C" void pc_mods_init(void) {
    Scan();
    auto* context = aurora::rmlui::get_context();
    if (!context) {
        pc_log_line("mods: no RmlUi context; the characters menu is unavailable");
        return;
    }
    g_menu.doc = context->LoadDocumentFromMemory(kDocument, "[mods]");
    if (!g_menu.doc) {
        pc_log_line("mods: couldn't build the characters menu");
        return;
    }
    g_menu.doc->AddEventListener(Rml::EventId::Click, &g_menu);
}

extern "C" void pc_mods_css_enter(void) {
    g_css = true;
    InvalidateLoaded();
}

extern "C" void pc_mods_css_exit(void) {
    g_css = false;
    g_menu.SetOpen(false);
    InvalidateLoaded(); /* the next scene loads fresh models into its own heap */
}

extern "C" bool pc_mods_menu_open(void) {
    return g_css && g_menu_open;
}

extern "C" void pc_mods_scene_reset(void) {
    InvalidateLoaded();
}

extern "C" void* pc_mods_fighter_joint(int player, int kind, void* base_joint) {
    if (player < 0 || player >= kPorts || pc_net_active())
        return nullptr;
    int ch = g_port[player];
    if (ch < 0 || ch >= (int)g_chars.size() || g_chars[ch]->kind != kind)
        return nullptr; /* no character, or this player picked a different fighter */
    Loaded& l = g_loaded[player];
    if (l.valid && l.ch == ch && l.kind == kind && l.base == base_joint)
        return l.joint;
    const Character& c = *g_chars[ch];
    l = Loaded();
    void* archive = nullptr;
    void* joint = nullptr;
    void* matanim = nullptr;
    if (!c.matanim.empty()) {
        lbArchive_80017040(&archive, c.gameName.c_str(), &joint, c.joint.c_str(), &matanim, c.matanim.c_str(),
            nullptr);
    } else {
        lbArchive_80017040(&archive, c.gameName.c_str(), &joint, c.joint.c_str(), nullptr);
    }
    if (!joint) {
        pc_log_line("mods: P%d: couldn't load %s; using %s's own model", player + 1, c.folder.c_str(),
            kKinds[kind].name);
        return nullptr;
    }
    l.valid = true;
    l.ch = ch;
    l.kind = kind;
    l.base = base_joint;
    l.archive = archive;
    l.joint = joint;
    l.matanim = matanim;
    pc_log_line("mods: P%d plays %s (on %s)", player + 1, c.name.c_str(), kKinds[kind].name);
    return joint;
}

extern "C" bool pc_mods_fighter_matanim(int player, int kind, void** matanim) {
    if (player < 0 || player >= kPlayerMax)
        return false;
    const Loaded& l = g_loaded[player];
    if (!l.valid || l.kind != kind)
        return false;
    *matanim = l.matanim;
    return true;
}

extern "C" void pc_mods_update(void) {
    if (!g_menu.doc)
        return;
    static bool f2Was = false;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool f2 = keys && keys[SDL_SCANCODE_F2];
    if (f2 && !f2Was && g_css)
        g_menu.SetOpen(!g_menu_open);
    f2Was = f2;
    /* the icon shows only on character select, and not under the F1 settings overlay */
    bool show = g_css && !pc_menu_is_open();
    if (show && !g_menu.doc->IsVisible())
        g_menu.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    else if (!show && g_menu.doc->IsVisible()) {
        g_menu.SetOpen(false);
        g_menu.doc->Hide();
    }
}
