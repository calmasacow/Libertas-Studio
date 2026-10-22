<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR-0067: Retain and preload inactive media

Status: Accepted

## Context

A real background MP4 takes roughly two seconds to produce its first frame through
our Qt source worker. The renderer currently destroys unseen workers at every scene
change. OBS retains local media unless close_when_inactive is enabled, preloads a
frame, and separates activation/restart from decoder construction. Decoder startup
must not recur on every cut.

## Decision

Media and playlists expose restartOnActivate (default true) and closeWhenInactive
(default false). OBS import/export preserves both flags. Existing imported sources
with only obs.original recover these two settings from that retained metadata.

The renderer prepares retained media on collection load, retains workers while their
source exists, and tracks visibility independently. Source-worker activation travels
as a private settings value in the existing project transport, not saved user state.
Unseen restarting media pauses after preloading its first frame; activation restarts
playback without constructing another QMediaPlayer. Non-restarting media continues
its timeline while hidden, but contributes no program audio. Explicit close-on-hide
releases the worker and therefore incurs startup next time. Deletion/settings edits
release or replace retained workers. Hidden sources do not drive program refresh.

## Limits and validation

Preloading trades bounded decoder/frame memory per media source for fast switches;
this does not eliminate initial collection warmup or make unavailable files ready.
Checks drive worker activation and the real renderer, verify retained first pixels,
inactive audio gating, restart semantics and removal. Measure cold and warm switches
using the operator's actual background file without editing the saved collection.
