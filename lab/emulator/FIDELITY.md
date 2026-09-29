# Synthetic emulator fidelity

The maintained emulator now provides only local recording and streaming state
transitions, events, and explicit JSON persistence. These are development
fixtures; they do not model the TI8168 media board, vendor control protocol,
firmware web API, recording files, media transport, or physical behavior.

The runtime intentionally rejects incompatible persisted state formats.
Validation of physical behavior requires separately authorized hardware work.
