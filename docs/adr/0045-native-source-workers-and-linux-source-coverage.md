# ADR 0045 - Native source workers and Linux source coverage

- Status: accepted
- Date: 2026-10-19

## Decision

Keep the existing scene walk and Vulkan/CPU executors. Source workers supply straight RGBA
frames through the existing sequenced-packet, sealed-memory preview transport. Capture keeps
its BGRA path; frame descriptors state channel order explicitly. Neither worker nor UI
implements scene placement or blending.

Text, slideshow, media and browser sources run outside the renderer. Qt's installed text,
image, multimedia and WebEngine facilities provide their native implementations; libobs is
not linked. A worker has one source and a bounded output size. Worker polling is asynchronous,
unchanged content sends only acknowledgement, and loss of a worker clears source availability.
Stopping or changing a source closes and reaps its worker. Pooling is deferred until process
startup or memory measurements justify sharing a failure domain.

Visual worker sources expose an explicit output width and height, also used by the editor's
transform handles. Text wraps within that box. Media and slides preserve aspect ratio inside
it with transparent margins. The same box reaches both compositors. Every visual source uses
the existing context-menu and transform commands, including unavailable sources whose size
is configured. Unsupported options are not presented as functioning controls.

Linux window and game capture use the compositor's window capture interface when available;
game capture does not claim an injected graphics hook. Audio input and desktop output capture
use the native sound server. Blackmagic is optional at runtime and requires the vendor driver
and a device. macOS-only capture is excluded. VLC playlists use the native media pipeline when
their formats are supported; there is no dependency on libVLC.

OBS import follows functioning source backends. It maps source identities, scene references,
groups, ordering, visibility and transforms, retaining diagnostics for unsupported filters,
transitions, capture identities and platform-only types. Import creates a new project and never
rewrites the OBS collection. Source compatibility is reported per imported item.

## Validation

Checks drive the worker process and shared compositor, including pixels, transparency,
updates, missing files, shutdown and transform behavior. Device-dependent checks are explicit;
an absent device is not evidence of a working hardware backend. Builds and the full suite
must pass before each section is committed and pushed.
