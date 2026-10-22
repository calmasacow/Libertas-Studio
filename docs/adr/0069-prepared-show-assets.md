<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR-0069: Prepared show assets on local storage

Status: Proposed

## Context

A collection references media and stills by their original paths. The operator's
Marathon show reads all of them through a gvfs (SMB) mount. A stat on that mount
measured 10-14 ms at times and stalled a frame once a second until caches stopped
statting inline (fac75f6). A network hiccup during a show can still delay a file's
first read or a worker's open. Formats are also whatever the asset was authored in:
PNG decodes on first use, the 4K H.264 trailer needs 6 seconds to become ready, and
WebM alpha clips decode in software through libvpx (ADR-0066).

## Decision

A "Prepare show" action converts every asset the collection references into a
prepared copy under a local cache directory
(`$XDG_CACHE_HOME/libertas/prepared/`, overridable per machine). The collection is
not rewritten: sources keep their original paths, and the renderer and workers use
the prepared copy when one exists and is current. Deleting the cache directory returns
the show to reading originals, so preparation is never required for correctness.

Each prepared file is keyed by a hash of the original's content plus the preparation
recipe version, with an index mapping original path to key. A changed original or a
new recipe produces a new key; stale entries are removed by the next preparation, not
during a show. Lookups go through the in-memory index and do not stat the original
on the render thread.

Stages, each usable alone:

1. **Local copy.** Byte-identical copy of every referenced file. Removes network reads
   from the show.
2. **Stills.** Decoded straight sRGB8 pixels, memory-mapped from the cache, so first use
   needs no PNG/JPEG/WebP decode. An optional per-source BC7 encoding (GPU-native,
   4x smaller in VRAM and on upload) is offered, not default, because it is lossy and
   can soften thin text on overlays.
3. **Video.** Transcode to a hardware-decodable form when the original is not one:
   H.264/HEVC through NVDEC or VA-API. Alpha clips become packed alpha (colour above,
   alpha below, in one H.264/HEVC frame) and are unpacked by a compute shader. Original
   frame rate and resolution are kept unless the operator chooses otherwise.
4. **Resident clips.** Stingers and short loops, below a per-clip size limit, are stored
   as GPU-compressed frames and loaded into VRAM when the collection opens, so they
   start with no decode and no I/O. Long media stays a compressed stream.

Conversion runs in a separate process with FFmpeg and the project's existing image
decoders. It reports per-asset results, and failures leave that asset on its original.

## Consequences

The cache can be large: a resident 3.5 s 1080p stinger is about 220 MB. The action
shows the projected size before converting and refuses to fill the disk. Lossy stages
(BC7 stills, transcoded video) are per-source choices with a preview, never silent.
Preparation can be slow (transcoding is minutes for long video) and is an explicit
operator action, not something that runs as a show opens.

## Limits and validation

Checks: a prepared still renders the same pixels as its original on CPU and GPU; a
packed-alpha transcode matches the WebM original within a stated tolerance; replacing
an original invalidates its entry; a missing or corrupt cache entry falls back to the
original with a diagnostic; the renderer performs no stat or open on an original path
during a show when every asset is prepared.
