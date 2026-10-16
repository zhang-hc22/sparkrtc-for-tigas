/*
 *  TIGAS: client of tigas/webrtc/render_worker.py over a Unix socket.
 */

#include "examples/tigas/render_client.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace tigas {

namespace {
constexpr size_t kHeaderSize = 20;
}  // namespace

RenderClient::~RenderClient() {
  if (fd_ >= 0)
    close(fd_);
}

bool RenderClient::Connect(const std::string& path, int timeout_s) {
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    RTC_LOG(LS_ERROR) << "Render socket path too long: " << path;
    return false;
  }
  memcpy(addr.sun_path, path.data(), path.size());

  const int64_t deadline = rtc::TimeMillis() + timeout_s * 1000;
  while (true) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      RTC_LOG(LS_ERROR) << "socket(AF_UNIX) failed: " << strerror(errno);
      return false;
    }
    if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      RTC_LOG(LS_INFO) << "Connected to render worker at " << path;
      return true;
    }
    int err = errno;
    close(fd_);
    fd_ = -1;
    if (rtc::TimeMillis() > deadline) {
      RTC_LOG(LS_ERROR) << "Cannot connect to render worker at " << path
                        << ": " << strerror(err);
      return false;
    }
    usleep(200000);
  }
}

bool RenderClient::Render(const std::string& line, RenderReply* reply) {
  std::string request = line;
  request.push_back('\n');
  if (!WriteFull(request.data(), request.size()))
    return false;

  uint8_t header[kHeaderSize];
  if (!ReadFull(header, sizeof(header)))
    return false;
  memcpy(&reply->i, header, 4);
  memcpy(&reply->width, header + 4, 4);
  memcpy(&reply->height, header + 8, 4);
  memcpy(&reply->render_ms, header + 12, 8);
  reply->buffer = nullptr;
  if (reply->width == 0)
    return true;

  const int w = reply->width;
  const int h = reply->height;
  rtc::scoped_refptr<webrtc::I420Buffer> buffer =
      webrtc::I420Buffer::Create(w, h);
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  // I420Buffer::Create(w, h) uses strides w and (w + 1) / 2, so each plane is
  // one contiguous block.
  if (buffer->StrideY() != w || buffer->StrideU() != cw ||
      buffer->StrideV() != cw) {
    RTC_LOG(LS_ERROR) << "Unexpected I420Buffer strides";
    return false;
  }
  if (!ReadFull(buffer->MutableDataY(), static_cast<size_t>(w) * h) ||
      !ReadFull(buffer->MutableDataU(), static_cast<size_t>(cw) * ch) ||
      !ReadFull(buffer->MutableDataV(), static_cast<size_t>(cw) * ch))
    return false;
  reply->buffer = buffer;
  return true;
}

void RenderClient::Shutdown() {
  if (fd_ >= 0)
    shutdown(fd_, SHUT_RDWR);
}

bool RenderClient::ReadFull(void* dst, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  while (n > 0) {
    ssize_t r = recv(fd_, p, n, 0);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      RTC_LOG(LS_ERROR) << "Render worker read failed: "
                        << (r == 0 ? "closed" : strerror(errno));
      return false;
    }
    p += r;
    n -= r;
  }
  return true;
}

bool RenderClient::WriteFull(const void* src, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(src);
  while (n > 0) {
    ssize_t r = send(fd_, p, n, MSG_NOSIGNAL);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      RTC_LOG(LS_ERROR) << "Render worker write failed: " << strerror(errno);
      return false;
    }
    p += r;
    n -= r;
  }
  return true;
}

}  // namespace tigas
