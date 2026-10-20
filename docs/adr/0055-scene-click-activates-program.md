# ADR-0055: Scene clicks activate Program

Status: accepted

## Decision

Left-clicking a scene in Scenes or Favourites selects it and switches its
canvas's Program scene immediately. The shared activation action validates the
scene, finds its canvas's deck, and uses the existing SetDeckProgram command.
Clicking the already active scene adds no undo entry. Right-click selects for
context actions without switching Program. Stars and rename controls keep their
existing actions. Selection APIs used internally remain selection-only.

Studio mode's separate preview/take controls remain explicit deck controls;
these scene-list clicks perform direct activation as requested.

Older secondary canvases may have no deck. AddDeck and RemoveDeck provide
undoable, journaled deck lifecycle commands. First activation creates a deck
with the chosen Program scene; subsequent clicks reuse it. Canvas removal
removes its decks in the same batch as its encodes and destinations, after
the existing scene-empty requirement is met. Project schema is unchanged.

## Checks

Drive real clicks in both panels, including a secondary canvas, and assert
Program state and rendering. Check repeated clicks, right-click behavior,
selection, and undo. Deck commands must round-trip through the journal format.
