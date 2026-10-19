# ADR-0049: Output Manager edits shared encodes and destination credentials

Status: accepted

## Decision

Output Manager selects existing encodes by ID, not display name. Codec, bitrate,
rate control, keyframe interval, and preset edit the existing EncoderConfig through
undoable document commands. Shared destinations see the same changes. No schema
change is required; unsupported encoder-specific options are not invented.

Destination settings expose Name, URL, and a password-masked replacement Key.
Keys never enter document commands, journals, project JSON, or logs. The Linux
Secret Service stores them through the installed `secret-tool` utility, with the
secret supplied on stdin, never a command argument. Saving is asynchronous and
bounded by a timeout. A missing helper, locked/unavailable store, or failed save
reports failure without changing the credential reference. A successful save
sets a generated credential reference. Existing keys are not fetched into QML.

## Consequences

Key changes are external to document undo. Copies carrying the same reference
use the same credential. Removing an output does not delete its credential,
since another project or undo history may still reference it. Encoding remains
unavailable until the output engine is connected; these controls configure it.
