// SPDX-License-Identifier: MIT
/*
 * xlt's command line: `xlt run <elf> [--stats]`.
 *
 * The command runs a static AArch64 ELF through the translator and returns its exit code. Errors
 * opening or translating the image are reported on stderr and map to exit status 1; a command
 * line mistake maps to 2, matching the scaffold's convention.
 */
#include "xlt.h"

#include <cstdio>
#include <cstring>

namespace
{

const char kUsage[] = "usage: xlt run <elf> [--stats]\n"
                      "       xlt --version\n"
                      "       xlt --help\n"
                      "  --stats   print translation/execution counters\n";

int usage(FILE *out)
{
    std::fputs(kUsage, out);
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage(stderr);
    }

    const char *cmd = argv[1];
    if (std::strcmp(cmd, "--help") == 0 || std::strcmp(cmd, "-h") == 0 ||
        std::strcmp(cmd, "help") == 0) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (std::strcmp(cmd, "--version") == 0) {
        std::printf("xlt %s\n", xlt::Engine::version());
        return 0;
    }
    if (std::strcmp(cmd, "run") != 0) {
        std::fprintf(stderr, "xlt: unknown command '%s'\n", cmd);
        return usage(stderr);
    }

    const char *elf = nullptr;
    bool stats = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stats") == 0) {
            stats = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "xlt: unknown option '%s'\n", argv[i]);
            return usage(stderr);
        } else if (elf == nullptr) {
            elf = argv[i];
        } else {
            std::fprintf(stderr, "xlt: run takes exactly one ELF path\n");
            return usage(stderr);
        }
    }
    if (elf == nullptr) {
        std::fprintf(stderr, "xlt: run needs an ELF path\n");
        return usage(stderr);
    }

    xlt::Engine engine;
    std::string error;
    if (!engine.load(elf, &error)) {
        std::fprintf(stderr, "xlt: %s\n", error.c_str());
        return 1;
    }

    xlt::RunOptions options;
    const int exit_code = engine.run(options, &error);
    if (exit_code < 0) {
        std::fprintf(stderr, "xlt: %s\n", error.c_str());
        return 1;
    }

    if (stats) {
        const xlt::Stats &s = engine.stats();
        std::fprintf(stderr,
                     "xlt: stats blocks-translated=%llu blocks-executed=%llu "
                     "insns-translated=%llu insns-executed=%llu\n",
                     static_cast<unsigned long long>(s.blocks_translated),
                     static_cast<unsigned long long>(s.blocks_executed),
                     static_cast<unsigned long long>(s.instructions_translated),
                     static_cast<unsigned long long>(s.instructions_executed));
    }
    return exit_code;
}
