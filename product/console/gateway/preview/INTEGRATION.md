# Preview media-core integration contract

This directory is a transport-agnostic, video-only RTSP/RTP reader and
H.264-to-FLV mux. It does not authenticate HTTP requests, open sockets, create
threads, select policy, or emit HTTP headers. Those remain gateway/FastCGI
responsibilities.

## Build inventory

The canonical source list lives once in
[`sources.mk`](sources.mk), included by `../../Makefile`. It compiles the
local preview media core (`preview_reader.c`, `preview_reader_protocol.c`,
`preview_reader_rtp.c`, `preview_flv.c`) together with the maintained sipd
parser/depacketizer implementations, reused at build time without local
copies: `../../../sipd/src/backends/rtsp_parser.c`,
`../../../sipd/src/media/h264.c`, and
`../../../sipd/src/media/h264_depacketizer.c`. `../../tests/preview/run.sh`
reads the same list via `make -s -C product/console print-preview-sources`
instead of repeating it.

Add include roots `gateway/preview`, `../sipd/include`, and
`../sipd/src/backends`. The code is C99-compatible and is tested with the
repository's C17 warning policy.

## Worker sequence

1. Authenticate the session and viewer role, validate CSRF/origin policy, and
   reserve the single preview slot before creating a reader.
2. Obtain the binary IPv4 address and port only from the strict operator-owned
   policy. Do not derive either from HTTP input. The reader accepts no URI or
   path and always requests `/movie`.
3. Create one reader per independent RTSP/TCP connection. Send each request
   returned by `start`, `push`, or `drain` completely, with a caller-owned
   monotonic deadline. Never log requests because PLAY contains a session ID.
4. Feed bounded socket reads to `push`, then call `drain` with no input until it
   returns `AULA_STATUS_AGAIN`. A returned access unit remains valid only until
   the next reader call.
5. Wait for the first returned IDR before sending an HTTP 200 response. Failure,
   timeout, unsupported packetization, or EOF before that point can therefore
   remain a truthful bounded JSON 503 response.
6. After the HTTP headers, initialize one FLV mux and pass each access unit to
   `aula_preview_flv_mux_write`. Its callback must synchronously consume the
   complete byte span. Any callback or mux error is terminal: close the HTTP and
   RTSP connections and release the preview slot.
7. Revalidate the session lease and stream deadline between bounded reads. The
   media core intentionally retains neither session pointers nor credentials.

There is no RTSP TEARDOWN API. `aula_preview_reader_eof` marks local state
closed; normal cancellation, expiry, parser failure, or peer EOF is completed
by closing the independent TCP socket. A reconnect always creates a fresh
reader, which prevents parser, SSRC, parameter-set, and timestamp state from
crossing connections.

## Fixed bounds and fail-closed behavior

- RTSP parser storage is the maintained fixed 16,388-byte parser; SDP bodies
  are limited to 8,192 bytes and interleaved RTP packets to 1,500 bytes.
- Access-unit capacity is caller-configured from 4 KiB through 2 MiB. The
  reader owns one assembly buffer and the maintained depacketizer owns one
  equally bounded reconstruction buffer.
- SPS and PPS are each capped at 1,024 bytes. FLV output is emitted in bounded
  stack spans and is never retained as a complete stream or tag.
- Only RTP/TCP interleaved channels 0 and 1 are accepted. Channel 0 must carry
  the SDP-selected H.264 payload; channel 1 RTCP is ignored.
- Sequence gaps, SSRC changes, timestamp regressions, large timestamp jumps,
  and parameter-set changes clear decoder readiness. P frames are dropped until
  a new SPS/PPS-backed IDR is available. RTP timestamp wrap is unwrapped.
- Single-NAL and FU-A packetization are supported. Other RFC 6184 aggregation
  modes fail closed.
- SPS/PPS are emitted as AVCDecoderConfigurationRecord data. Other NAL payload
  bytes are unchanged. B slices and data-partition/extension slices return
  `AULA_STATUS_UNSUPPORTED` with
  `AULA_PREVIEW_FLV_ERROR_UNSUPPORTED_TIMING`; no zero-CTS claim is made.

Run `product/console/tests/preview/run.sh` for the socket-free protocol, recovery,
byte-preservation, timing, and golden-FLV tests.
