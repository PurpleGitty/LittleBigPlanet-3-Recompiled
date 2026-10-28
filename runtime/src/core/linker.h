// Resolves imports between loaded modules and our HLE replacements, and
// applies relocations. Anything nobody provides gets a logging stub.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/module.h"

namespace core {

struct LinkStats {
    size_t relocations = 0;
    size_t fromModules = 0, fromHle = 0, stubbedFuncs = 0, stubbedData = 0;
    std::map<std::string, std::vector<std::string>> missing;  // library -> names/NIDs
};

class Linker {
public:
    Linker();

    // Makes `fn` the implementation of `name` in PS4 library `lib`.
    void RegisterHle(const std::string& lib, const std::string& name, void* fn);
    // Same, for exports whose name isn't known (only the NID is).
    void RegisterHleNid(const std::string& lib, const std::string& nid, void* fn);
    // Imports from `from` that nothing provides are looked up in `to` as well
    // (e.g. libSceLibcInternal -> the game's own libc).
    void AddLibraryAlias(const std::string& from, const std::string& to);
    // Optional: names for NIDs, used only to make reports readable.
    void AddNameHint(const std::string& name);

    void AddModule(Module* m);
    // Links every added module. Call after all modules are added.
    void LinkAll();

    const LinkStats& Stats() const { return stats_; }
    const std::vector<Module*>& Modules() const { return modules_; }

    struct Stub { std::string lib, name; uint64_t calls = 0; };

private:

    void Relocate(Module& m);
    uint64_t ResolveImport(const Module& m, const ElfSym& sym);
    uint64_t MakeStub(const std::string& lib, const std::string& name);
    uint64_t CountingThunk(const std::string& lib, const std::string& name, uint64_t target);
    std::string Pretty(const std::string& nid) const;

    std::vector<Module*> modules_;
    std::unordered_map<std::string, uint64_t> exports_;  // "NID#lib" -> address
    std::unordered_map<std::string, uint64_t> hle_;      // "NID#lib" -> address
    std::unordered_map<std::string, std::string> names_; // NID -> name
    std::unordered_map<std::string, std::string> aliases_;  // library -> fallback library
    std::unordered_map<std::string, uint64_t> stubCache_;  // "name#lib" -> thunk
    std::unordered_map<std::string, uint64_t> countThunks_;  // "nid#lib" -> counting thunk
    std::set<std::string> reported_;                       // "name#lib" already in stats_.missing
    std::deque<Stub> stubs_;
    std::deque<std::vector<uint8_t>> dataStubs_;
    uint8_t* thunkArena_ = nullptr;
    size_t thunkUsed_ = 0;
    uint64_t nextTlsOffset_ = 0;
    uint32_t nextTlsIndex_ = 1;
    LinkStats stats_;
};

// Call counting (LBP3_COUNT_CALLS): zero the counters / print the top entries.
void ResetCallCounts();
void PrintCallCounts(int top);

}  // namespace core
