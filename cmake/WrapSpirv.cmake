# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Wraps glslc's -mfmt=c output in a header. Script mode, invoked by libertas_compile_shader.
file(READ "${INC}" _words)
string(STRIP "${_words}" _words)
file(WRITE "${OUT}"
"// Generated from a .comp by libertas_compile_shader. Do not edit.\n"
"#pragma once\n"
"#include <cstdint>\n"
"inline constexpr std::uint32_t ${SYM}[] = ${_words};\n")
file(REMOVE "${INC}")
