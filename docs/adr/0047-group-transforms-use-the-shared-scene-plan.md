# ADR 0047 - Group transforms use the shared scene plan

- Status: accepted
- Date: 2026-10-19

## Decision

Identity groups retain existing pass-through ordering and child opacity semantics. A transformed
group resolves its children into a transparent canvas-sized draw plan, then places that plan
with the same scale, flip and quarter-turn rules used for scene sources. Both executors consume
the same operation. Its output box is the owning canvas, like a default nested scene; content
outside that box is clipped. Tight automatic group bounds can follow if operators need them.

The editor exposes that box to the existing context menu and transforms. Child handles map
through ancestor group transforms; drag/resize requests map back into parent coordinates before
changing document state. Ungrouping a transformed group must preserve placement or refuse with
a clear explanation; silently dropping the parent's transform is forbidden.

Checks cover group scale and translation in both executors and editor manipulation of nested
children. Unsupported skew/arbitrary rotation remains reported instead of approximated.
