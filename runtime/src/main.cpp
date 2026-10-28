// lbp3.exe [<patch dir> <base dir>] [names file] [--run] [--dump-after=SECONDS] [--break=module+0xoffset ...] [--skip-intro]
//
// Without the two dirs, the game files are found in the game folder (GameDir below), which
// holds CUSA00473 (base game) and CUSA00473-patch; the names file defaults to
// tools\nid_names.txt.
//
// Loads eboot.bin plus every module it needs that ships with the game, links
// them against each other and our HLE libraries, and reports what's missing.
// With --run, starts the game.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/debugger.h"
#include "core/linker.h"
#include "core/module.h"
#include "core/runner.h"
#include "core/tls.h"
#include "core/vfs.h"
#include "gpu/renderer.h"
#include "video/presenter.h"

#include <SDL3/SDL_keycode.h>
#include "hle/hle.h"

namespace fs = std::filesystem;

namespace {

// Folders inside the app/patch dirs where the game keeps its own modules.
const char* const kModuleDirs[] = {"sce_module", "gamedata_orbis/spu", "."};

fs::path FindModule(const std::vector<fs::path>& roots, const std::string& name) {
    for (const auto& root : roots)
        for (const char* dir : kModuleDirs)
            if (fs::path p = root / dir / name; fs::exists(p)) return p;
    return {};
}

// The folder holding the user's own game dump, kept outside the project so the project
// can be shared: LBP3_GAME_DIR, else the first line of game_dir.txt in the working folder,
// else "..\LBP3 Game Files" next to the project.
fs::path GameDir() {
    if (const char* v = std::getenv("LBP3_GAME_DIR"); v && *v) return v;
    if (std::ifstream f("game_dir.txt"); f) {
        std::string line;
        std::getline(f, line);
        while (!line.empty() && static_cast<unsigned char>(line.back()) <= ' ') line.pop_back();  // CR, spaces
        if (!line.empty()) return line;
    }
    return fs::current_path().parent_path() / "LBP3 Game Files";
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--gpu-test") return gpu::Init() ? 0 : 1;  // device bring-up only
    }
    std::vector<fs::path> roots;  // patch files win over base files
    int first = 1;
    if (argc >= 3 && argv[1][0] != '-' && fs::is_directory(argv[1]) && fs::is_directory(argv[2])) {
        roots = {argv[1], argv[2]};
        first = 3;
    } else {
        const fs::path game = GameDir();
        roots = {game / "CUSA00473-patch", game / "CUSA00473"};
        if (!fs::exists(roots[1] / "eboot.bin")) {
            std::fprintf(stderr,
                         "game files not found in %s\n(that folder should hold CUSA00473 and CUSA00473-patch; "
                         "set it in game_dir.txt or LBP3_GAME_DIR)\n",
                         game.string().c_str());
            return 2;
        }
    }
    bool run = false;
    std::vector<std::string> breakpoints;
    std::string namesFile = fs::exists("tools/nid_names.txt") ? "tools/nid_names.txt" : "";
    for (int i = first; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--run") run = true;
        else if (a.rfind("--break=", 0) == 0) breakpoints.push_back(a.substr(8));
        else if (a.rfind("--dump-after=", 0) == 0) core::SetDumpAfter(std::stoi(a.substr(13)));
        else if (a == "--skip-intro") core::vfs::SetSkipIntro(true);
        else namesFile = a;
    }

    try {
        core::Linker linker;
        hle::RegisterAll(linker);
        if (!namesFile.empty()) {
            std::ifstream names(namesFile);
            for (std::string line; std::getline(names, line);)
                if (!line.empty()) linker.AddNameHint(line);
        }

        std::vector<std::unique_ptr<core::Module>> modules;
        std::set<std::string> seen;
        std::vector<std::string> system;
        std::vector<std::string> queue = {"eboot.bin"};
        while (!queue.empty()) {
            std::string name = queue.back();
            queue.pop_back();
            if (!seen.insert(name).second) continue;
            fs::path path = FindModule(roots, name);
            if (path.empty()) { system.push_back(name); continue; }
            auto m = core::Module::Load(path);
            std::printf("loaded %-28s %-24s at %p (%zu KB)\n", m->fileName.c_str(),
                        m->moduleName.c_str(), static_cast<void*>(m->base), m->size / 1024);
            for (const auto& n : m->needed) queue.push_back(n);
            linker.AddModule(m.get());
            modules.push_back(std::move(m));
        }

        core::InitTls(linker.Modules());
        for (auto& m : modules) {
            if (size_t n = core::PatchFsAccesses(*m))
                std::printf("patched %zu fs:[0] reads in %s\n", n, m->fileName.c_str());
        }
        linker.LinkAll();
        core::debugger::Arm(breakpoints, linker.Modules());
        const auto& s = linker.Stats();
        std::printf("\n%zu relocations applied\n", s.relocations);
        std::printf("imports: %zu from game modules, %zu from HLE, %zu functions stubbed, %zu variables stubbed\n",
                    s.fromModules, s.fromHle, s.stubbedFuncs, s.stubbedData);
        std::printf("%zu system modules expected from the OS\n", system.size());

        size_t total = 0;
        for (const auto& [lib, names] : s.missing) total += names.size();
        std::printf("\nmissing (%zu unique):\n", total);
        for (const auto& [lib, names] : s.missing) {
            std::printf("  %-32s %3zu:", lib.c_str(), names.size());
            for (size_t i = 0; i < names.size() && i < 4; ++i) std::printf(" %s", names[i].c_str());
            std::printf("%s\n", names.size() > 4 ? " ..." : "");
        }

        if (run) {
            // /app0 reads look in mods/ first, then the patch, then the base game.
            const fs::path root = fs::current_path();
            core::vfs::Init({root / "mods", roots[0], roots[1]}, root / "user");
            std::fflush(stdout);
            // The game loads spu.prx and the FMOD plugins itself later on; these two
            // are started up front like the PS4 kernel does. Dependencies first.
            std::vector<core::Module*> startFirst;
            for (const char* name : {"libSceFios2.prx", "libc.prx"})
                for (auto& m : modules)
                    if (m->fileName == name) startFirst.push_back(m.get());
            // Debug hotkeys in the game window: F9 dumps the next frame's render
            // targets and draw list, F10 writes a performance report.
            video::SetHotkeyHandler([](int key) {
                if (key == SDLK_F9) gpu::RequestTargetDump();
                if (key == SDLK_F10) core::RequestPerfReport();
            });
            core::Run(*modules.front(), startFirst, linker.Modules());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
