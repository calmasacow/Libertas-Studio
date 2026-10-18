# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The render process. Today it publishes a preview of each canvas and nothing else: no device,
# no encoder, no output. ADR-0005 puts those here when they exist; ADR-0040 is why the preview
# is already a separate process rather than a function the UI calls.

add_executable(libertas-render
    main.cpp
)

target_link_libraries(libertas-render PRIVATE
    libertas::flags
    libertas::lcomp
)
