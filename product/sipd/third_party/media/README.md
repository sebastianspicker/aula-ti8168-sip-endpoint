# Native media optional dependencies

The production RTSP path is native C and has no GStreamer, ALSA, package
manager, or runtime-download dependency.  Its AAC receive lane is intentionally
unavailable by default.

To enable it, a reviewed source build of **FAAD2 2.11.2** and **SpeexDSP
1.2.1** must be passed explicitly to CMake with both enable options, both
include roots, and both library paths.  CMake does not call `find_package`,
consult `pkg-config`, or download either dependency.  This keeps an accidental
host codec from changing appliance behavior.  The adapter accepts AAC-LC only,
downmixes/resamples to 8 kHz mono, and reserves the SpeexDSP AEC lane; exact
vendor capture/render hardware remains a separate validated integration.
