/*
 *  TIGAS: client of tigas/webrtc/render_worker.py over a Unix socket.
 *
 *  Request: one JSON line, {"i": <viewpoint>, "body": {<the /render body>}}.
 *  Reply: a 20-byte little-endian header (int32 i, uint32 width,
 *  uint32 height, float64 render_ms) followed by width * height * 3 / 2
 *  bytes of I420 (Y, then U, then V, no padding). width == 0 means the
 *  worker could not render this request and no pixel data follows.
 */

#ifndef EXAMPLES_TIGAS_RENDER_CLIENT_H_
#define EXAMPLES_TIGAS_RENDER_CLIENT_H_

#include <stdint.h>

#include <string>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"

namespace tigas {

struct RenderReply {
  int32_t i = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  double render_ms = 0;
  rtc::scoped_refptr<webrtc::I420Buffer> buffer;
};

class RenderClient {
 public:
  ~RenderClient();

  // Retries until the worker listens on `path` or `timeout_s` passes.
  bool Connect(const std::string& path, int timeout_s);
  // Sends `line` (no trailing newline) and blocks for the reply. Returns
  // false on a socket error; a worker-side failure returns true with
  // reply->width == 0.
  bool Render(const std::string& line, RenderReply* reply);
  // Unblocks a pending Render() from another thread.
  void Shutdown();

 private:
  bool ReadFull(void* dst, size_t n);
  bool WriteFull(const void* src, size_t n);

  int fd_ = -1;
};

}  // namespace tigas

#endif  // EXAMPLES_TIGAS_RENDER_CLIENT_H_
