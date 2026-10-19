# ADR 0044 - Video capture uses V4L2 and leased host frames

- Status: accepted
- Date: 2026-10-18

## Decision

`source.v4l2` uses its existing device, resolution, frameRate and colourRange settings.
The device selector stores a stable `/dev/v4l/by-id` path where available. Enumeration excludes
metadata nodes. Leave unchanged retains the device's current dimensions and rational frame rate;
explicit requests are checked against the negotiated mode instead of silently accepting a downgrade.

`lcapture` owns native V4L2 streaming with mmap buffers on a capture thread. This first slice
supports uncompressed NV12, YUYV, YUV420 and BGR24/RGB24 devices. Installed libswscale converts
these to BGRA8 using the negotiated range and colour matrix. Compressed-only and HDR modes
report unsupported format; video audio capture is a separate future pipeline.

The shared scene walk requests a frame by source descriptor. The renderer keeps one leased
snapshot per source per commit, so repeated placements and canvases see identical pixels.
The CPU consumes BGRA; the existing GPU image upload path consumes the same pixels. There is
no claim of zero-copy USB capture. A small host buffer pool bounds retained frames; busy buffers
are never overwritten. Disconnects clear availability and report an error, and stop joins the
capture thread before releasing mapped kernel buffers. Conflicting sources using the same device
report device busy rather than changing the mode of a running source.

Unsupported buffering and automatic-reset controls are omitted from the editor until implemented.
Hardware checks exercise the capture thread and real renderer process on the attached Elgato;
ordinary headless checks retain the unavailable-device and CPU fallback behavior.
