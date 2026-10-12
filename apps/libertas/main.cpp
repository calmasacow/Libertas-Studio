// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe.hpp"

#include <lcore/log.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::puts(R"(libertas - live streaming and recording studio

usage:
  libertas probe [--strict]   report this machine's capture/encode/theme environment
  libertas --version          print version
  libertas --help             this message

Not yet implemented (see the plan's milestones):
  libertas run                launch the session supervisor and UI
  libertas attach             attach a UI to a running session)");
}

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();

    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty()) {
        usage();
        return 1;
    }

    const std::string_view cmd = args.front();

    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-V") {
        std::puts("libertas " LIBERTAS_VERSION);
        return 0;
    }
    if (cmd == "probe") {
        bool strict = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--strict") {
                strict = true;
            } else {
                lc::error("probe: unknown option '{}'", args[i]);
                return 1;
            }
        }
        return libertas::probe_main(strict);
    }

    lc::error("unknown command '{}' (try --help)", cmd);
    return 1;
}
