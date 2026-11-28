// SPDX-License-Identifier: MIT
/*
 * engine.cpp — the `xlt run` front end: ELF loading, stack setup, CLI host callbacks and the
 * summary line. Translation and dispatch live in libxlt.cpp so QEMU can embed the same engine.
 */
#include "xlt.h"
#include "libxlt.h"
#include "lift.h"

#include <algorithm>
#include <cerrno>
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

[[noreturn]] void cli_fatal(void *, const char *message, int exit_code)
{
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
    std::_Exit(exit_code);
}

uint64_t cli_cntvct(void *)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 62500000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 16ull;
}

constexpr uint64_t kPl011Base = 0x09000000;
constexpr uint64_t kPl011Size = 0x1000;
constexpr uint64_t kPl011Dr = 0x00;
constexpr uint64_t kPl011Fr = 0x18;

uint64_t cli_read_mem(void *, uint64_t addr, unsigned)
{
    if (addr >= kPl011Base && addr < kPl011Base + kPl011Size) {
        if (addr - kPl011Base == kPl011Fr) {
            return 0x90; /* TXFE | RXFE, same value the PL011 reports to the P2.1 fixture */
        }
        return 0;
    }
    cli_fatal(nullptr, ("xlt: bad guest memory access at " + hex(addr)).c_str(), 70);
}

void cli_write_mem(void *, uint64_t addr, uint64_t value, unsigned)
{
    if (addr >= kPl011Base && addr < kPl011Base + kPl011Size) {
        if (addr - kPl011Base == kPl011Dr) {
            std::fputc(static_cast<int>(value & 0xff), stdout);
            std::fflush(stdout);
        }
        return;
    }
    cli_fatal(nullptr, ("xlt: bad guest memory access at " + hex(addr)).c_str(), 70);
}

uint64_t cli_read_sysreg(void *, uint32_t reg)
{
    switch (reg) {
    case 0xDF02: /* CNTVCT_EL0 */
        return cli_cntvct(nullptr);
    case 0xDF00: /* CNTFRQ_EL0 */
        return 62500000ull;
    case 0xC005: /* MPIDR_EL1 */
        return 0;
    default:
        return 0;
    }
}

void cli_write_sysreg(void *, uint32_t, uint64_t)
{
    /* The CLI has no EL model; DAIF/TPIDR writes are accepted and ignored. */
}

void cli_hvc(void *, CpuRegs *regs, uint32_t imm)
{
    const uint64_t function = regs->x[0];
    if (imm == 0 && (function == 0x84000008ull || function == 0x84000009ull)) {
        /* PSCI SYSTEM_OFF / SYSTEM_RESET: exit successfully, like the QEMU machine. */
        regs->halted = 1;
        regs->exit_code = 0;
        return;
    }
    cli_fatal(nullptr, ("xlt: unsupported HVC/SMC at pc " + hex(regs->pc)).c_str(), 71);
}

} // namespace

struct Engine::Impl {
    std::unique_ptr<uint8_t[]> ram;
    std::unique_ptr<LibXlt> lib;
    HostOps host;
    uint64_t entry = 0;
    uint64_t image_end = kRamBase;
    bool loaded = false;

    static void semihost(void *opaque, CpuRegs *regs)
    {
        Engine::Impl *impl = static_cast<Engine::Impl *>(opaque);
        const uint64_t op = regs->x[0];
        const uint64_t arg = regs->x[1];
        switch (op) {
        case 0x04: { /* SYS_WRITE0 */
            if (impl->ram == nullptr || arg < kRamBase || arg - kRamBase >= kRamSize) {
                break;
            }
            const char *text = reinterpret_cast<const char *>(impl->ram.get() + (arg - kRamBase));
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
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() = default;

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

    impl_->lib.reset();
    impl_->ram = std::move(ram);
    impl_->entry = eh.e_entry;
    impl_->image_end = image_end;
    impl_->loaded = true;

    impl_->host = HostOps{};
    impl_->host.opaque = impl_.get();
    impl_->host.ram = impl_->ram.get();
    impl_->host.ram_base = kRamBase;
    impl_->host.ram_size = kRamSize;
    impl_->host.read_mem = cli_read_mem;
    impl_->host.write_mem = cli_write_mem;
    impl_->host.read_sysreg = cli_read_sysreg;
    impl_->host.write_sysreg = cli_write_sysreg;
    impl_->host.semihost = Impl::semihost;
    impl_->host.hvc = cli_hvc;
    impl_->host.cntvct = cli_cntvct;
    impl_->host.fatal = cli_fatal;

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

    impl_->host.ram = impl_->ram.get();
    impl_->lib = std::make_unique<LibXlt>(impl_->host);
    if (!impl_->lib->init(error)) {
        return -1;
    }
    const int rc = impl_->lib->run(&regs_, options.max_instructions, options.trace, &stats_, error);
    if (rc != 0) {
        return -1;
    }

    std::fprintf(stderr,
                 "xlt: wall=%llu ns translate=%llu ns opt+jit=%llu ns regions=%llu insns=%llu "
                 "dispatches=%llu exit=%llu\n",
                 static_cast<unsigned long long>(impl_->lib->wall_ns()),
                 static_cast<unsigned long long>(impl_->lib->translate_ns()),
                 static_cast<unsigned long long>(impl_->lib->opt_jit_ns()),
                 static_cast<unsigned long long>(stats_.blocks_translated),
                 static_cast<unsigned long long>(stats_.instructions_executed),
                 static_cast<unsigned long long>(impl_->lib->dispatches()),
                 static_cast<unsigned long long>(regs_.exit_code));
    std::fflush(stderr);

    if (regs_.exit_code > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(regs_.exit_code);
}

} // namespace xlt
