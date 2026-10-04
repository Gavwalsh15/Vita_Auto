#pragma once

namespace video { struct DecodedFrame; }

namespace video {
namespace presenter {

// Main thread owns GXM initialization and shutdown.
void init();
// False means display cleanup did not complete; do not start another session.
bool shutdown();
// Main-thread fallback, while the decoder's picture mutex is held.
bool presentCpuFallback(const video::DecodedFrame& frame);
// Main thread only, before streaming or after all session workers have joined.
void presentStatus(const char* title, const char* detail, const char* error,
                   const char* action, const char* audio);

} // namespace presenter
} // namespace video
