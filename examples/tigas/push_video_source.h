/*
 *  TIGAS: video source fed by the render worker, one frame per viewpoint.
 */

#ifndef EXAMPLES_TIGAS_PUSH_VIDEO_SOURCE_H_
#define EXAMPLES_TIGAS_PUSH_VIDEO_SOURCE_H_

#include "absl/types/optional.h"
#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "media/base/adapted_video_track_source.h"

namespace tigas {

// Screen-content source, the same as the localvideo example
// (examples/peerconnection/localvideo/conductor.cc), so the encoder keeps the
// resolution under congestion. Push() skips AdaptFrame(): every rendered
// viewpoint reaches the encoder, and the sink wants (resolution and frame
// rate limits from the encoder) are ignored.
class PushVideoSource : public rtc::AdaptedVideoTrackSource {
 public:
  // Safe to call from any thread.
  void Push(const webrtc::VideoFrame& frame) { OnFrame(frame); }

  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return true; }
  absl::optional<bool> needs_denoising() const override { return true; }
};

}  // namespace tigas

#endif  // EXAMPLES_TIGAS_PUSH_VIDEO_SOURCE_H_
