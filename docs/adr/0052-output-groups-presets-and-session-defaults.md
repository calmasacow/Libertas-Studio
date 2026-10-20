# ADR-0052: Output groups, portable presets, and session defaults

Status: accepted

## Decision

Output Manager presents one named block per Encode, with its canvas, encoder
settings, and editable destination list. The Outputs dock remains a list of
individual destinations. Add destination changes only that list; Add output
creates an encode and one unconfigured destination. Removing an encoder group
removes its destinations in one undoable batch. Removing a destination leaves
its encode available in the manager, including when no destinations remain.
Internal Encode and Output IDs and project schema remain unchanged.

Presets are independent snapshots of encoder settings and output scaling, kept
in the user's application config directory. Applying a preset copies settings;
later library edits cannot change existing outputs. Presets contain no canvas
identity, destination identity, URL, or credential. Writes are atomic; malformed
or unreadable libraries are reported and never silently overwritten. Built-in
16:9 and 9:16 defaults are always available in new sessions. User presets can be
saved by name, explicitly overwritten, applied, and removed.

New UI sessions contain Main canvas, Main Output encode, and one disabled,
unconfigured Main destination. New canvases select horizontal or vertical and
create a matching encode/destination in the same undo step. Existing projects
are not rewritten on load. Removing a canvas remains prohibited while it holds
scenes; once empty its encodes and destinations are removed with it in one step.

Both defaults: 60 fps, H.264 x264, 6000 kbps CBR, 2000 ms keyframes, medium CPU
preset, custom buffer disabled (6000 retained), Main profile, Film tune, AAC at
160 kbps, track 1, VOD track 0. Horizontal canvas is 1920x1080; vertical is
1080x1920. Default encodes target the corresponding 1080 resolution. Applying
these presets to larger canvases scales down; smaller canvases are not upscaled.
Canvas geometry and scene composition are never changed by an encoder preset.

## Consequences

Operator names identify output groups, independently of destination and preset
names. Existing encode names remain editable. Encoder settings work even with
zero destinations. The generic ldoc document factory remains minimal for library
consumers; the UI session factory supplies the configured studio starting state.
The output engine is still separate work; this configures intended output only.
