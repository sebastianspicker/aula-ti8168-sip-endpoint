# Synthetic emulator fidelity

The maintained emulator now provides only local recording and streaming state
transitions, events, and explicit JSON persistence. These are development
fixtures; they do not model the TI8168 media board, vendor control protocol,
firmware web API, recording files, media transport, or physical behavior.

The former vendor-compatible modules and tests are preserved in the private
source snapshot under `evidence/private/vendor-compat/`. The public runtime
intentionally rejects their persisted state format. Validation of physical
behavior requires separately authorized evidence and hardware work.
