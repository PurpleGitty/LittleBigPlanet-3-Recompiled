// A PS4 ELF (eboot.bin or .prx) mapped into this process.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

struct ElfSym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
    uint8_t Type() const { return info & 0xF; }
    uint8_t Bind() const { return info >> 4; }
};

struct ElfRela {
    uint64_t offset, info;
    int64_t addend;
    uint32_t Type() const { return static_cast<uint32_t>(info); }
    uint32_t Sym() const { return static_cast<uint32_t>(info >> 32); }
};

struct TlsInfo {
    uint64_t vaddr = 0, fileSize = 0, memSize = 0, align = 1;
    uint32_t index = 0;   // module ID for DTPMOD64, 0 = no TLS
    uint64_t offset = 0;  // distance below the thread pointer (static TLS, variant II)
};

class Module {
public:
    // Reads, maps and parses the file. Throws std::runtime_error on malformed input.
    static std::unique_ptr<Module> Load(const std::filesystem::path& path);
    ~Module();

    // Applies final page protections once relocation is done.
    void Protect();

    // Executable segments as (address, size), for code patching before Protect().
    std::vector<std::pair<uint8_t*, size_t>> CodeRanges() const;
    bool Contains(const void* addr) const {
        auto* p = static_cast<const uint8_t*>(addr);
        return p >= base && p < base + size;
    }

    const char* Str(uint32_t off) const;
    uint8_t* Addr(uint64_t vaddr) const { return base + vaddr; }

    std::string fileName;
    std::string moduleName;               // from DT_SCE_MODULE_INFO
    uint8_t* base = nullptr;
    size_t size = 0;
    uint64_t entry = 0;                   // vaddr
    uint64_t procParam = 0;               // vaddr of PT_SCE_PROCPARAM (eboot only)
    TlsInfo tls;
    std::vector<std::string> needed;      // DT_NEEDED file names
    std::unordered_map<uint16_t, std::string> importLibs, exportLibs;
    std::vector<ElfSym> syms;
    std::vector<ElfRela> relas;           // DT_SCE_RELA + DT_SCE_JMPREL
    bool started = false;                 // init has run
    bool hasInit = false;                 // DT_INIT present (its vaddr is often 0)
    uint64_t init = 0;                    // DT_INIT vaddr
    uint64_t initArray = 0;               // DT_INIT_ARRAY vaddr; entries are filled by relocation
    uint64_t initArrayCount = 0;

private:
    struct Segment { uint64_t vaddr, memSize; uint32_t flags; };
    std::vector<uint8_t> file_;
    uint64_t strtab_ = 0, strsz_ = 0;
    std::vector<Segment> segments_;
};

}  // namespace core
