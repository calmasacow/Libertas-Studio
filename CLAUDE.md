<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Libertas-Studio — agent notes

A panel change is one QML file. Do not load the repo, the ADRs, or a screenshot into the thread to make it.

## Where to look

- Panels and their widgets: `apps/libertas-ui/qml/`. A panel is `<Name>Panel.qml`. Dialogs and small controls sit beside it.
- Behaviour that is not in the QML: search `apps/libertas-ui/` for the type or the command, then read a line range. `main.cpp` is 152KB and `document.cpp` is 127KB. Never read either whole.
- Document commands: `libs/ldoc/`. Layout math: `libs/llayout/`.
- Headless check, from the repo root: `./build/linux/apps/libertas-ui/libertas-ui --check`. Report the failing assertion, not the run.

## What stays out of the thread

- Do not read `docs/adr/` or `docs/todo.md` unless the user names that file. There are twenty-eight ADRs, about 318KB. The README already states the decision that matters for UI work.
- Do not read `build/` or `build-*/`. That tree is generated, and it is where full-window captures have been accumulating.
- Do not read a capture that is wider or taller than 1280px, or larger than 80KB. Scale it, then read the scaled file once:

```sh
magick input.png -resize '1280x1280>' -quality 60 /tmp/libertas-view.jpg
```

- Long command output (configure, build, ctest, find) goes through `tail -n 40`. If the full log matters, write it under `/tmp` and read a range.
- Do not keep going into neighbouring panels after the requested one is done.
