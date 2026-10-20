# ADR-0054: Coalesce preview refreshes and own frame storage

Status: accepted

## Decision

Keep at most one pending live refresh while a preview request is in flight.
On acknowledgment, send the latest document if dirty, otherwise send that one
pending refresh. Do not wait another timer interval or build a backlog. Stopping
live preview clears the pending refresh. Use Qt's precise timer for preview ticks.

The UI transfers ownership of decoded pixel storage to QImage, whose cleanup
callback releases it after the last image reference goes away. Images never alias
the renderer's reusable shared memory. This removes a full-image copy while
preserving immutable frames across asynchronous Qt image-provider use.

## Evidence and limits

Live profiling found GPU work mostly 4-9 ms but request round trips of 15-66 ms
with a large preview. Discarding timer ticks adds another wait after delayed
acknowledgments. CPU samples are dominated by image copying.

Unchanged-capture skipping remains enabled. This remains a request-driven preview,
not an autonomous encoder clock. Display refresh and compositor scheduling still
limit visible presentation cadence.
