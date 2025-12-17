/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/localvideo/main_wnd.h"

#include <math.h>

#include "api/video/i420_buffer.h"
#include "examples/peerconnection/client/defaults.h"
#include "rtc_base/arraysize.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_argb.h"

extern bool is_sender;
extern std::string recon_filename;

MainWnd::MainWnd(const char* server,
                 int port,
                 bool auto_call)
    : ui_(CONNECT_TO_SERVER),
      wnd_(NULL),
      callback_(NULL),
      server_(server),
      auto_call_(auto_call) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%i", port);
  port_ = buffer;
}

MainWnd::~MainWnd() {
  RTC_DCHECK(!IsWindow());
}

bool MainWnd::Create() {
  RTC_DCHECK(wnd_ == NULL);
  ui_thread_id_ = ::GetCurrentThreadId();
  SwitchToConnectUI();

  return true;
}

bool MainWnd::Destroy() {
  BOOL ret = FALSE;
  if (IsWindow()) {
    ret = ::DestroyWindow(wnd_);
  }

  return ret != FALSE;
}

void MainWnd::RegisterObserver(MainWndCallback* callback) {
  callback_ = callback;
}

bool MainWnd::IsWindow() {
  return wnd_ && ::IsWindow(wnd_) != FALSE;
}

bool MainWnd::PreTranslateMessage(MSG* msg) {
  bool ret = false;
  if (msg->hwnd == NULL && msg->message == UI_THREAD_CALLBACK) {
    callback_->UIThreadCallback(static_cast<int>(msg->wParam),
                                reinterpret_cast<void*>(msg->lParam));
    ret = true;
  }
  return ret;
}

void MainWnd::SwitchToConnectUI() {
  ui_ = CONNECT_TO_SERVER;
}

void MainWnd::AutoConnect() {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " server: " << server_
                   << " port: " << port_;
  if (callback_) {
    int port = port_.length() ? atoi(port_.c_str()) : 0;
    callback_->StartLogin(server_, port);
  }
}

void MainWnd::SwitchToPeerList(const Peers& peers) {
  // typedef std::map<int, std::string> Peers;
  // loop and print all peers
  for (auto it = peers.begin(); it != peers.end(); ++it) {
    RTC_LOG(LS_INFO) << __FUNCTION__ << "peer id: " << it->first << " name: " << it->second;
  }
  // print peer size
  RTC_LOG(LS_INFO) << __FUNCTION__ << "peer size: " << peers.size();

  if(auto_call_ && peers.size() > 0) {
    int id = peers.begin()->first;
    callback_->ConnectToPeer(id);
  }
}

void MainWnd::SwitchToStreamingUI() {
  ui_ = STREAMING;
}

void MainWnd::MessageBox(const char* caption, const char* text, bool is_error) {
}

void MainWnd::StartLocalRenderer(webrtc::VideoTrackInterface* local_video) {
  local_renderer_.reset(new VideoRenderer(handle(), 1, 1, local_video));
}

void MainWnd::StopLocalRenderer() {
  local_renderer_.reset();
}

void MainWnd::StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video) {
  remote_renderer_.reset(new VideoRenderer(handle(), 1, 1, remote_video));
}

void MainWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
}

void MainWnd::QueueUIThreadCallback(int msg_id, void* data) {
  ::PostThreadMessage(ui_thread_id_, UI_THREAD_CALLBACK,
                      static_cast<WPARAM>(msg_id),
                      reinterpret_cast<LPARAM>(data));
}

//
// MainWnd::VideoRenderer
//

MainWnd::VideoRenderer::VideoRenderer(
    HWND wnd,
    int width,
    int height,
    webrtc::VideoTrackInterface* track_to_render)
    : wnd_(wnd), rendered_track_(track_to_render) {
  width_ = 0;
  height_ = 0;
  rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
  if (!is_sender) {
    file_ = fopen(recon_filename.c_str(), "wb");
  }
}

MainWnd::VideoRenderer::~VideoRenderer() {
  fclose(file_);
  rendered_track_->RemoveSink(this);
}

void MainWnd::VideoRenderer::SetSize(int width, int height) {
  if (width_ == width && height_ == height) {
    return;
  }
  width_ = width;
  height_ = height;
  image_.reset(new uint8_t[width * height * 4]);
}

void MainWnd::VideoRenderer::OnFrame(const webrtc::VideoFrame& video_frame) {
  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
    video_frame.video_frame_buffer()->ToI420());
  if (video_frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
  }
  SetSize(buffer->width(), buffer->height());

  if (!is_sender){
    RTC_LOG(LS_INFO) << __FUNCTION__ << " write to file";
    fwrite(buffer->DataY(), 1, buffer->width() * buffer->height(), file_);
    fwrite(buffer->DataU(), 1, buffer->width() * buffer->height() / 4, file_);
    fwrite(buffer->DataV(), 1, buffer->width() * buffer->height() / 4, file_);
  }

  libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                    buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                    image_.get(), width_ * 4, buffer->width(),
                    buffer->height());
}
