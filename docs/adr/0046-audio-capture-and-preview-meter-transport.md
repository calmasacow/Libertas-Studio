# ADR 0046 - Audio capture and preview meter transport

- Status: accepted
- Date: 2026-10-19

## Decision

Audio Input and Audio Output Capture use the PulseAudio client API, including PipeWire's
PulseAudio service. Output capture records the selected sink monitor; input records a source.
Devices are enumerated at runtime. Acquisition uses 48 kHz stereo float PCM and runs on the
sound server's threaded main loop, independently of video rendering. Capture buffers are bounded.

The renderer owns capture sessions and publishes measured source levels over preview protocol
v5 before each render acknowledgement, even when no video frame changes. Peaks are finite,
bounded values with explicit availability; disconnected sources do not masquerade as silence.
The UI displays these meters beside existing faders. PCM remains available through the capture
API for routing and output integration. Preview meters do not claim an encoder is running.

Media workers send bounded stereo float PCM packets through the same transport. The renderer
mixes 480-frame blocks independently of video: source gain/mute, bus gain/mute/pan, pre/post
fader sends, delays and explicit local monitoring use the existing document bus/send model.
Source meters are pre-fader; bus meters are post-fader. Unsupported enabled bus inserts mute
that bus and mark its meter unavailable. Encode/output handoff remains separate work.
Hardware checks record real server buffers; process checks decode a tone, mix it into a bus
and receive measured meters. Audio-only sources have no scene transform.
