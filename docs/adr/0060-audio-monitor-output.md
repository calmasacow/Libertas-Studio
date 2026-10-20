# ADR-0060: Explicit audio monitoring output

## Status

Accepted, 2026-10-20.

## Decision

Store `audio.monitor.output` in project settings extra data. Missing or malformed
values mean `none`, which is the default for new and existing collections. `default`
requests the audio server's default output; other values are PulseAudio/PipeWire
sink identities enumerated at runtime, stored as `sink:<identity>` to avoid
collisions with reserved None/System default values. These settings use the existing undoable,
journaled SetProjectSettings command and native project serialization.

The audio mixer footer offers None, System default and discovered output sinks,
with Refresh for device changes. Preserve an unavailable saved selection visibly;
never substitute a different named sink. Selecting None releases playback while
preserving source/bus monitoring flags, capture, metering and program routing.
Changing the output recreates only the monitoring playback stream. Named streams
cannot be moved automatically to another sink by the audio server. Failed opens
remain silent and retry at a bounded rate.

## Validation

Drive the real mixer selector and persistence/undo in the UI check. Exercise playback
and capture against an explicitly selected test null sink when audio hardware checks
are enabled, including rejection of a missing sink. No monitoring is enabled on the
operator's speakers by tests.
