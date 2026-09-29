# Single source of truth for the preview build's source list: the local
# preview media core plus the maintained sipd sources it reuses at build time
# (see ../preview/INTEGRATION.md). Included by ../../Makefile and exposed via
# `make -s -C product/console print-preview-sources` for cross-build tooling.
PREVIEW_SOURCES := \
	gateway/preview/preview_reader.c \
	gateway/preview/preview_reader_protocol.c \
	gateway/preview/preview_reader_rtp.c \
	gateway/preview/preview_flv.c \
	../sipd/src/backends/rtsp_parser.c \
	../sipd/src/media/h264.c \
	../sipd/src/media/h264_depacketizer.c
