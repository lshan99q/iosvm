// SPDX-License-Identifier: MIT
/*
 * engine.cpp — ELF loading, the block dispatcher and the host runtime helpers.
 *
 * Guest memory is one flat 256 MiB window mapped at kRamBase. The ELF's PT_LOAD segments are
 * copied into it, BSS is already zero, and generated code accesses it through a host pointer
 * embedded at translation time. A small direct-mapped cache turns the guest PC into a translated
 * function; a miss translates the region at that PC and installs it.
 */
#include "xlt.h"
#include "jit.h"
#include "lift.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <elf.h>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace xlt
{

namespace
{

uint8_t *g_ram = nullptr;

uint64_t now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

std::string hex(uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

} // namespace

struct Engine::Impl {
    std::unique_ptr<uint8_t[]> ram;
    std::unique_ptr<Jit> jit;
    uint64_t entry = 0;
    uint64_t image_end = kRamBase;
    bool loaded = false;
    uint64_t translated_ns = 0;
    uint64_t opt_jit_ns = 0;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine()
{
    if (impl_ != nullptr && g_ram == impl_->ram.get()) {
        g_ram = nullptr;
    }
}

Engine::Engine(Engine &&) noexcept = default;
Engine &Engine::operator=(Engine &&) noexcept = default;

bool Engine::load(const std::string &elf_path, std::string *error)
{
    std::ifstream file(elf_path, std::ios::binary);
    if (!file) {
        set_error(error, "cannot open " + elf_path);
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end < 0) {
        set_error(error, "cannot size " + elf_path);
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> image(static_cast<size_t>(end));
    if (!image.empty()) {
        file.read(reinterpret_cast<char *>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        if (!file) {
            set_error(error, "cannot read " + elf_path);
            return false;
        }
    }

    if (image.size() < sizeof(Elf64_Ehdr)) {
        set_error(error, elf_path + ": truncated ELF header");
        return false;
    }
    Elf64_Ehdr eh;
    std::memcpy(&eh, image.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
        set_error(error, elf_path + ": not an ELF64 file");
        return false;
    }
    if (eh.e_machine != EM_AARCH64) {
        set_error(error, elf_path + ": not an AArch64 ELF");
        return false;
    }
    if (eh.e_type != ET_EXEC) {
        set_error(error, elf_path + ": only ET_EXEC images are supported in M1");
        return false;
    }
    if (eh.e_phentsize < sizeof(Elf64_Phdr) || eh.e_phoff > image.size() ||
        static_cast<uint64_t>(eh.e_phnum) >
            (image.size() - static_cast<size_t>(eh.e_phoff)) / eh.e_phentsize) {
        set_error(error, elf_path + ": malformed program header table");
        return false;
    }

    auto ram = std::unique_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(kRamSize)]);
    std::memset(ram.get(), 0, static_cast<size_t>(kRamSize));
    uint64_t image_end = kRamBase;

    for (unsigned int i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        std::memcpy(&ph, image.data() + eh.e_phoff + static_cast<uint64_t>(i) * eh.e_phentsize,
                    sizeof(ph));
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
            continue;
        }
        if (ph.p_filesz > ph.p_memsz) {
            set_error(error, elf_path + ": PT_LOAD has filesz > memsz");
            return false;
        }
        if (ph.p_vaddr < kRamBase || ph.p_vaddr - kRamBase > kRamSize ||
            ph.p_memsz > kRamSize - (ph.p_vaddr - kRamBase)) {
            set_error(error, elf_path + ": PT_LOAD outside the 256 MiB guest window");
            return false;
        }
        if (ph.p_offset > image.size() || ph.p_filesz > image.size() - ph.p_offset) {
            set_error(error, elf_path + ": PT_LOAD extends past the end of the file");
            return false;
        }
        const uint64_t offset = ph.p_vaddr - kRamBase;
        std::memcpy(ram.get() + offset, image.data() + ph.p_offset,
                    static_cast<size_t>(ph.p_filesz));
        std::memset(ram.get() + offset + ph.p_filesz, 0,
                    static_cast<size_t>(ph.p_memsz - ph.p_filesz));
        image_end = std::max(image_end, ph.p_vaddr + ph.p_memsz);
    }

    if (eh.e_entry < kRamBase || eh.e_entry >= kRamBase + kRamSize || (eh.e_entry & 3u) != 0) {
        set_error(error, elf_path + ": entry point is not a valid guest address");
        return false;
    }

    impl_->ram = std::move(ram);
    impl_->entry = eh.e_entry;
    impl_->image_end = image_end;
    impl_->loaded = true;
    impl_->translated_ns = 0;
    impl_->opt_jit_ns = 0;
    g_ram = impl_->ram.get();

    regs_ = CpuRegs{};
    regs_.pc = impl_->entry;
    stats_ = Stats{};
    return true;
}

int Engine::run(const RunOptions &options, std::string *error)
{
    if (!impl_->loaded) {
        set_error(error, "no ELF image loaded");
        return -1;
    }
    if (impl_->jit == nullptr) {
        impl_->jit = std::make_unique<Jit>();
    }
    if (!impl_->jit->init(error)) {
        return -1;
    }

    regs_ = CpuRegs{};
    const uint64_t top = kRamBase + kRamSize;
    if (options.stack_size != 0) {
        if (options.stack_size > kRamSize || options.stack_size > (top - kRamBase)) {
            set_error(error, "guest stack is larger than the guest address space");
            return -1;
        }
        const uint64_t stack_base = top - options.stack_size;
        if (stack_base < impl_->image_end) {
            set_error(error, "guest stack overlaps the loaded image");
            return -1;
        }
        std::memset(impl_->ram.get() + (stack_base - kRamBase), 0,
                    static_cast<size_t>(options.stack_size));
        regs_.sp = (stack_base + options.stack_size - 16) & ~static_cast<uint64_t>(15);
    } else {
        regs_.sp = 0;
    }

    const uint64_t start = options.entry != 0 ? options.entry : impl_->entry;
    if (start < kRamBase || start >= kRamBase + kRamSize || (start & 3u) != 0) {
        set_error(error, "invalid run entry point " + hex(start));
        return -1;
    }

    regs_.halted = 0;
    regs_.exit_code = 0;
    regs_.pc = start;
    stats_ = Stats{};
    impl_->translated_ns = 0;
    impl_->opt_jit_ns = 0;

    struct DCacheEntry {
        uint64_t pc;
        BlockFn fn;
        uint64_t insns;
    };
    static DCacheEntry dc[1u << 13];
    for (auto &entry : dc) {
        entry.pc = std::numeric_limits<uint64_t>::max();
        entry.fn = nullptr;
        entry.insns = 0;
    }

    uint64_t pc = start;
    uint64_t translate_ns = 0;
    uint64_t opt_jit_ns = 0;
    uint64_t dispatches = 0;
    const uint64_t begin = now_ns();

    while (regs_.halted == 0) {
        DCacheEntry &slot = dc[(pc ^ (pc >> 12)) & 0x1fffu];
        if (slot.pc != pc || slot.fn == nullptr) {
            uint64_t insns = 0;
            const uint64_t t0 = now_ns();
            const BlockFn fn = impl_->jit->get(pc, impl_->ram.get(), &insns, error);
            const uint64_t t1 = now_ns();
            translate_ns += impl_->jit->timing().translate_ns;
            opt_jit_ns += impl_->jit->timing().opt_jit_ns;
            if (fn == nullptr) {
                if (error != nullptr && error->empty()) {
                    *error = "cannot translate pc " + hex(pc);
                }
                return -1;
            }
            if (insns == 0) {
                set_error(error, "empty region at " + hex(pc));
                return -1;
            }
            slot.pc = pc;
            slot.fn = fn;
            slot.insns = insns;
            stats_.blocks_translated++;
            stats_.instructions_translated += insns;
            if (options.trace) {
                std::fprintf(stderr, "xlt: translate 0x%llx (%llu insns) took %llu ns\n",
                             static_cast<unsigned long long>(pc),
                             static_cast<unsigned long long>(insns),
                             static_cast<unsigned long long>(t1 - t0));
            }
        }

        const BlockFn fn = slot.fn;
        const uint64_t insns = slot.insns;
        if (options.trace) {
            std::fprintf(stderr, "xlt: run 0x%llx\n", static_cast<unsigned long long>(pc));
        }
        const uint64_t next = fn(&regs_);
        stats_.blocks_executed++;
        stats_.instructions_executed += insns;

        if (options.max_instructions != 0 &&
            stats_.instructions_executed > options.max_instructions) {
            set_error(error, "instruction limit exceeded at " + hex(pc));
            return -1;
        }
        if (regs_.halted != 0) {
            break;
        }
        if (next == 0) {
            set_error(error, "block at " + hex(pc) + " returned no successor");
            return -1;
        }
        dispatches++;
        pc = next;
        regs_.pc = pc;
    }

    const uint64_t wall_ns = now_ns() - begin;
    impl_->translated_ns += translate_ns;
    impl_->opt_jit_ns += opt_jit_ns;
    std::fprintf(stderr,
                 "xlt: wall=%llu ns translate=%llu ns opt+jit=%llu ns regions=%llu insns=%llu "
                 "dispatches=%llu exit=%llu\n",
                 static_cast<unsigned long long>(wall_ns),
                 static_cast<unsigned long long>(translate_ns),
                 static_cast<unsigned long long>(opt_jit_ns),
                 static_cast<unsigned long long>(stats_.blocks_translated),
                 static_cast<unsigned long long>(stats_.instructions_executed),
                 static_cast<unsigned long long>(dispatches),
                 static_cast<unsigned long long>(regs_.exit_code));
    std::fflush(stderr);

    if (regs_.exit_code > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(regs_.exit_code);
}

} // namespace xlt

extern "C" void xlt_bad_mem(uint64_t addr, uint64_t size)
{
    std::fprintf(stderr, "xlt: bad guest memory access addr=0x%llx size=0x%llx\n",
                 static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
    std::fflush(stderr);
    std::_Exit(70);
}

extern "C" void xlt_unsupported(uint64_t pc, uint64_t insn)
{
    std::fprintf(stderr, "xlt: unsupported instruction 0x%08llx at 0x%llx\n",
                 static_cast<unsigned long long>(insn), static_cast<unsigned long long>(pc));
    std::fflush(stderr);
    std::_Exit(71);
}

extern "C" uint64_t xlt_cntvct()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 62500000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 16ull;
}

extern "C" void xlt_semihost(xlt::CpuRegs *regs)
{
    const uint64_t op = regs->x[0];
    const uint64_t arg = regs->x[1];
    switch (op) {
    case 0x04: { /* SYS_WRITE0: write the NUL-terminated string at x1. */
        if (xlt::g_ram == nullptr || arg < xlt::kRamBase || arg - xlt::kRamBase >= xlt::kRamSize) {
            break;
        }
        const char *text = reinterpret_cast<const char *>(xlt::g_ram + (arg - xlt::kRamBase));
        std::fputs(text, stdout);
        std::fflush(stdout);
        break;
    }
    case 0x18: /* SYS_EXIT */
        regs->halted = 1;
        regs->exit_code = arg;
        break;
    default:
        regs->halted = 1;
        regs->exit_code = 1;
        break;
    }
}
