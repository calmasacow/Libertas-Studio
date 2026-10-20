# ADR-0061: Preserve OBS canvas membership

## Status

Accepted, 2026-10-20.

## Decision

Translate each distinct OBS canvas UUID to a native canvas, using saved canvas
names. Main remains the operator-supplied canvas. Source sharing and cross-scene
references retain their existing project identities. Extra scenes keep their names
and belong to their translated canvas instead of being renamed and flattened.

OBS's canvas metadata stores identity, not video geometry. When custom-size scenes
on one canvas consistently specify dimensions, use those as its import geometry.
Reject conflicting sizes with an actionable error rather than choosing silently.
Where dimensions are absent, create a separate canvas at the supplied Main size
and issue an explicit review warning. FPS inherits the operator-supplied rate,
also reported. Do not guess portrait geometry from a canvas name.

Initialize an extra canvas's deck to its first non-group scene and report that
selection, because OBS metadata does not save its Program/Preview state here.
Global source records remain shared. Existing duplicate-audio warnings remain;
canvas mapping does not claim to solve multi-canvas audio summation.

Keep custom-size nested scenes' own dimensions when placing them. Groups without
canvas identities remain nested scenes on Main until group ownership is resolved
from explicit metadata; their saved tight bounds still govern nested rendering.

## Validation

Import, save/reopen and render multiple canvas scenes, checking dimensions,
membership, source sharing and independent Program selection. Reject conflicting
or oversized geometry before writing a destination. Preserve active Main selection
when extra canvases contain identically named scenes.
