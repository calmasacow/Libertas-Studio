# ADR-0050: Encoder-specific output configuration

Status: accepted

## Decision

Output Manager distinguishes an encoder configuration (a shared bitstream) from
its video encoder implementation. Show configuration ownership and sharing;
allow an output to copy its shared configuration into an independent encode.
The copy and reassignment form one undo step. Generated configuration names
continue to describe resolution, frame rate, and bitrate; users can rename them.

Expose NVENC and x264 settings conditionally. Store implementation-specific
options under `nvenc.*` and `x264.*` in EncoderConfig.extra. Common quality,
rescale filter, and audio configuration use `quality`, `rescale_filter`, and
`audio.*` keys. Scalars have allowlisted keys, enum choices, and numeric bounds
at the QML command boundary. Unknown existing extras remain intact. Native
rescale, preset, rate control, and keyframe fields remain authoritative.

Audio configuration includes codec, bitrate, main track and optional Twitch VOD
track (0 means disabled). These are requested output settings, not evidence of
an active audio encoder or provider support. No schema migration is needed.

## Consequences

Changing a shared configuration changes its destinations; Make independent
copies all settings before reassignment. Settings persist through existing
SetEncode commands, undo, and serialization. No custom option string is executed
as a shell command. The future output engine must validate device/codec support
and interpret these settings before starting. UI continues to state that output
encoding is unavailable; unsupported backend-specific controls are omitted.
