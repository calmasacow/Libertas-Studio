# ADR-0059: Shared preview blend modes

## Status

Accepted, 2026-10-19.

## Context

The document already stores blend modes, but both preview backends reject all
except Normal. OBS import discards them and the UI offers modes that cannot draw.
Marathon needs Screen and Multiply; the operator requests the full OBS menu set.

## Decision

Support Normal, Add, Subtract, Screen, Multiply, Lighten and Darken in the shared
draw plan, CPU executor and GPU compute shader. Add uses the existing serialized
`linear-dodge` value, with the UI label Add and OBS spelling `additive`. No schema
change is needed. Offer these seven working modes in Properties; retain other
document enum values and report them as unsupported when encountered.

Blend straight scene-linear RGB, then composite with source-over coverage. With
backdrop b and source s, the functions are s, min(1,b+s), max(0,b-s),
1-(1-b)*(1-s), b*s, max(b,s) and min(b,s), respectively. Transparent backdrop
contributes no blend color; transparent source changes nothing. Opacity changes
source coverage. This defines native alpha behavior consistently, including
transparent intermediate images. OBS fixed-function blending can differ for
partially transparent layers and additive/subtractive alpha; import/export reports
this limitation for non-Normal modes rather than claiming exact equivalence.

Flatten groups with non-Normal modes or explicit isolation before blending them
once. Group opacity applies once to the intermediate. Preserve the established
pass-through behavior of untransformed, non-isolated Normal groups. Reject explicit
pass-through with a non-Normal group mode rather than silently changing its meaning.
Clipping and unimplemented effects remain reported gaps.

## Consequences

No second scene walk or GPU readback is introduced. Normal retains its opaque copy
fast path. CPU's existing sRGB8 intermediates may differ from GPU RGBA16F by rounding;
tests compare real scenes within the existing two-byte tolerance. Regression checks
cover all modes, source alpha, opacity, nested/transformed groups, imported scenes
and export/reimport. Transform handles use the same supported-mode predicate.
