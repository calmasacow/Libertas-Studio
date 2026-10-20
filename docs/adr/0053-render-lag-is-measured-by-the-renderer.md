# ADR-0053: Render lag is measured by the renderer

Status: accepted

## Decision

The preview UI's busy timer ticks are not rendering lag. A request remains in
flight through audio work, transport, image copying and UI event dispatch.
Counting those ticks can report a missed frame for every successful render.

The renderer owns a cumulative per-canvas count of frame budgets consumed beyond
the one budget allotted to each composite. For duration D and canvas period T,
add max(0, ceil(D / T) - 1). Use the same measured interval as average render time,
including preview scaling/readback. Use each canvas's exact rational frame rate.
Unchanged-source acknowledgments and time outside compositing add nothing.

Protocol version 6 carries this count with cumulative render time and completed
frames. Reset stores a baseline for all three renderer counters. Encoding state
does not affect them.

## Limits

This measures preview-composite overruns, not end-to-end frame delivery or source
capture loss. The preview remains request-driven; a future autonomous output
scheduler must report actual missed output deadlines separately. A slow UI or
transport can lower preview throughput without increasing this render counter.
