#include "core/module.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace core {
namespace {

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
constexpr uint32_t PT_SCE_RELRO = 0x61000010, PT_SCE_DYNLIBDATA = 0x61000000;
constexpr uint32_t PT_SCE_PROCPARAM = 0x61000001;
constexpr uint32_t PF_X = 1, PF_W = 2;

constexpr int64_t DT_NEEDED = 1, DT_INIT = 12, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27;
constexpr int64_t DT_SCE_MODULE_INFO = 0x6100000D, DT_SCE_EXPORT_LIB = 0x61000013;
constexpr int64_t DT_SCE_IMPORT_LIB = 0x61000015, DT_SCE_JMPREL = 0x61000029;
constexpr int64_t DT_SCE_PLTRELSZ = 0x6100002D, DT_SCE_RELA = 0x6100002F;
constexpr int64_t DT_SCE_RELASZ = 0x61000031, DT_SCE_STRTAB = 0x61000035;
constexpr int64_t DT_SCE_STRSZ = 0x61000037, DT_SCE_SYMTAB = 0x61000039;
constexpr int64_t DT_SCE_SYMTABSZ = 0x6100003F;

struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, fileSize, memSize, align;
};

template <typename T>
T Read(const std::vector<uint8_t>& f, uint64_t off) {
    if (off + sizeof(T) > f.size()) throw std::runtime_error("read past end of file");
    T v;
    std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

DWORD ToWinProtect(uint32_t flags) {
    if (flags & PF_X) return (flags & PF_W) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return (flags & PF_W) ? PAGE_READWRITE : PAGE_READONLY;
}

}  // namespace

std::unique_ptr<Module> Module::Load(const std::filesystem::path& path) {
    auto m = std::make_unique<Module>();
    m->fileName = path.filename().string();

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    m->file_.assign(std::istreambuf_iterator<char>(in), {});
    const auto& f = m->file_;
    if (f.size() < 64 || std::memcmp(f.data(), "\x7F" "ELF", 4) != 0)
        throw std::runtime_error(m->fileName + " is not a decrypted ELF");

    m->entry = Read<uint64_t>(f, 24);
    uint64_t phoff = Read<uint64_t>(f, 32);
    uint16_t phnum = Read<uint16_t>(f, 56);

    std::vector<Phdr> phdrs(phnum);
    for (int i = 0; i < phnum; ++i) phdrs[i] = Read<Phdr>(f, phoff + i * sizeof(Phdr));

    // Map every loadable segment into one contiguous block at the vaddrs the file asks for.
    uint64_t span = 0;
    for (const auto& p : phdrs)
        if (p.type == PT_LOAD || p.type == PT_SCE_RELRO) span = std::max(span, p.vaddr + p.memSize);
    if (span == 0) throw std::runtime_error(m->fileName + " has no loadable segments");
    m->size = (span + 0xFFFF) & ~uint64_t{0xFFFF};
    // The PS4 always maps the main executable at 0x400000, and game code relies
    // on it: LBP3's spu.prx keeps 32-bit copies of pointers to eboot globals.
    // Libraries (PRX) can go anywhere.
    constexpr uint16_t kEtSceDynExec = 0xFE10;
    constexpr uintptr_t kExecBase = 0x400000;
    if (Read<uint16_t>(f, 16) == kEtSceDynExec) {
        m->base = static_cast<uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(kExecBase), m->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!m->base)
            std::fprintf(stderr, "warning: 0x400000 is taken; %s loads elsewhere and 32-bit pointers will break\n",
                         m->fileName.c_str());
    }
    if (!m->base)
        m->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, m->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m->base) throw std::runtime_error("out of memory mapping " + m->fileName);

    const Phdr* dyn = nullptr;
    const Phdr* dynlib = nullptr;
    for (const auto& p : phdrs) {
        switch (p.type) {
        case PT_LOAD:
        case PT_SCE_RELRO:
            if (p.offset + p.fileSize > f.size()) throw std::runtime_error(m->fileName + ": truncated segment");
            std::memcpy(m->base + p.vaddr, f.data() + p.offset, p.fileSize);
            m->segments_.push_back({p.vaddr, p.memSize, p.type == PT_SCE_RELRO ? 4u : p.flags});
            break;
        case PT_DYNAMIC: dyn = &p; break;
        case PT_SCE_DYNLIBDATA: dynlib = &p; break;
        case PT_SCE_PROCPARAM: m->procParam = p.vaddr; break;
        case PT_TLS: m->tls = {p.vaddr, p.fileSize, p.memSize, std::max<uint64_t>(p.align, 1)}; break;
        }
    }
    if (!dyn || !dynlib) throw std::runtime_error(m->fileName + " has no dynamic info");

    // The SCE dynamic tables point into PT_SCE_DYNLIBDATA, which is never mapped.
    const uint64_t dl = dynlib->offset;
    uint64_t symtab = 0, symtabSz = 0, rela = 0, relaSz = 0, jmprel = 0, jmprelSz = 0;
    uint64_t init = 0, initArray = 0, initArraySz = 0;
    bool hasInit = false;
    std::vector<uint64_t> neededOffs, moduleInfo, exportLib, importLib;
    for (uint64_t off = dyn->offset; off + 16 <= dyn->offset + dyn->fileSize; off += 16) {
        int64_t tag = Read<int64_t>(f, off);
        uint64_t val = Read<uint64_t>(f, off + 8);
        if (tag == 0) break;
        switch (tag) {
        case DT_NEEDED: neededOffs.push_back(val); break;
        case DT_INIT: init = val; hasInit = true; break;
        case DT_INIT_ARRAY: initArray = val; break;
        case DT_INIT_ARRAYSZ: initArraySz = val; break;
        case DT_SCE_MODULE_INFO: moduleInfo.push_back(val); break;
        case DT_SCE_EXPORT_LIB: exportLib.push_back(val); break;
        case DT_SCE_IMPORT_LIB: importLib.push_back(val); break;
        case DT_SCE_STRTAB: m->strtab_ = dl + val; break;
        case DT_SCE_STRSZ: m->strsz_ = val; break;
        case DT_SCE_SYMTAB: symtab = dl + val; break;
        case DT_SCE_SYMTABSZ: symtabSz = val; break;
        case DT_SCE_RELA: rela = dl + val; break;
        case DT_SCE_RELASZ: relaSz = val; break;
        case DT_SCE_JMPREL: jmprel = dl + val; break;
        case DT_SCE_PLTRELSZ: jmprelSz = val; break;
        }
    }

    // ID tables: value = id << 48 | version << 32 | name offset
    for (uint64_t v : neededOffs) m->needed.emplace_back(m->Str(static_cast<uint32_t>(v)));
    for (uint64_t v : moduleInfo) m->moduleName = m->Str(static_cast<uint32_t>(v));
    for (uint64_t v : exportLib) m->exportLibs[static_cast<uint16_t>(v >> 48)] = m->Str(static_cast<uint32_t>(v));
    for (uint64_t v : importLib) m->importLibs[static_cast<uint16_t>(v >> 48)] = m->Str(static_cast<uint32_t>(v));

    for (uint64_t off = symtab; off + sizeof(ElfSym) <= symtab + symtabSz; off += sizeof(ElfSym))
        m->syms.push_back(Read<ElfSym>(f, off));
    for (uint64_t off = rela; off + sizeof(ElfRela) <= rela + relaSz; off += sizeof(ElfRela))
        m->relas.push_back(Read<ElfRela>(f, off));
    for (uint64_t off = jmprel; off + sizeof(ElfRela) <= jmprel + jmprelSz; off += sizeof(ElfRela))
        m->relas.push_back(Read<ElfRela>(f, off));

    m->hasInit = hasInit;
    m->init = init;
    m->initArray = initArray;
    m->initArrayCount = initArraySz / 8;
    return m;
}

Module::~Module() {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
}

void Module::Protect() {
    for (const auto& s : segments_) {
        DWORD old;
        VirtualProtect(base + s.vaddr, s.memSize, ToWinProtect(s.flags), &old);
    }
    FlushInstructionCache(GetCurrentProcess(), base, size);
}

std::vector<std::pair<uint8_t*, size_t>> Module::CodeRanges() const {
    std::vector<std::pair<uint8_t*, size_t>> out;
    for (const auto& s : segments_)
        if (s.flags & PF_X) out.emplace_back(base + s.vaddr, s.memSize);
    return out;
}

const char* Module::Str(uint32_t off) const {
    if (off >= strsz_) return "";
    return reinterpret_cast<const char*>(file_.data() + strtab_ + off);
}

}  // namespace core
