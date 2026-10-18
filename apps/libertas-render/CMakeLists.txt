# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The render process. It publishes a preview of each canvas and captures the screens that preview
# names; no encoder and no output yet. ADR-0005 puts those here when they exist, and puts the GPU
# here, which is why the device lives in this process rather than in lcomp. ADR-0040 is why the
# preview is a separate process rather than a function the UI calls.

add_executable(libertas-render
    main.cpp
)

target_link_libraries(libertas-render PRIVATE
    libertas::flags
    libertas::lcomp
    libertas::lcapture
    libertas::lrhi
    libertas::lcore
)
