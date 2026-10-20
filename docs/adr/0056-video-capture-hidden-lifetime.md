# ADR-0056: Keep video capture ready between scene changes

Status: accepted

## Decision

V4L2 sources expose `deactivateWhenHidden`, default false, labelled "Deactivate
when not showing". Configured devices start when the project loads, including
sources in scenes that have never been shown. Capture continues off scene and retains its
latest frame. Returning to the scene reuses the running device without startup
black frames. Capture ends when the project removes the source or the renderer exits.

Enabling this option releases capture after the last visible placement disappears.
Removing a source or changing device, resolution, frame rate or colour range also
releases its session. Changing only the lifetime option does not restart capture.
Hidden capture updates do not trigger composites. Settings use the existing source
settings map; no project schema change is needed.

## Evidence and checks

Renderer `done()` unconditionally erased captures absent from the scene walk.
Live logs show repeated Elgato opens after scene switches. Exercise the real
renderer with the attached device: keep its descriptor while hidden, return with
a complete first composite, opt into release, and remove a hidden source.
