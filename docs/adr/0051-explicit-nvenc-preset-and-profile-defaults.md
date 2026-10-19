# ADR-0051: Explicit NVENC defaults

Status: accepted

NVENC configurations default to preset P5. H.264 defaults to High profile;
HEVC and AV1 default to Main. Preset and profile menus offer concrete values,
not an ambiguous encoder-default entry.

Existing empty values resolve to these defaults without rewriting a project on
read. The next encoder edit materializes defaults in the existing preset and
extra fields. Explicit selections remain unchanged. No schema migration is
needed. Changing codec resets the NVENC profile to that codec's default.
