/*
 *  TIGAS sender: renders each viewpoint the page sends over the DataChannel
 *  and streams the frames as one video track (x264, GCC).
 */

#include "examples/tigas/tigas_sender.h"

#include <inttypes.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "absl/strings/match.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/rtp_parameters.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/units/time_delta.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "modules/audio_device/include/fake_audio_device.h"
#include "p2p/client/basic_port_allocator.h"
#include "rtc_base/logging.h"
#include "rtc_base/strings/json.h"
#include "rtc_base/time_utils.h"

namespace tigas {

namespace {

// Milliseconds from the NTP epoch (1900) to the Unix epoch.
constexpr int64_t kNtpJan1970Ms = 2208988800000LL;

double WallMs() {
  return rtc::TimeUTCMicros() / 1000.0;
}

double MonoMs() {
  return rtc::TimeMicros() / 1000.0;
}

std::string ToJsonString(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

// Lists only the networks whose address starts with the prefix. Dropping the
// other candidates from the signaling (OnIceCandidate) is not enough: the ICE
// agent still checks from them, and the page then nominated the pair on the
// host's public address in the middle of the warm-up, which reset GCC.
class PrefixNetworkManager : public rtc::BasicNetworkManager {
 public:
  PrefixNetworkManager(rtc::SocketFactory* socket_factory, std::string prefix)
      : rtc::BasicNetworkManager(socket_factory), prefix_(std::move(prefix)) {}

  std::vector<const rtc::Network*> GetNetworks() const override {
    std::vector<const rtc::Network*> out;
    for (const rtc::Network* n : rtc::BasicNetworkManager::GetNetworks()) {
      if (absl::StartsWith(n->GetBestIP().ToString(), prefix_))
        out.push_back(n);
    }
    return out;
  }

 private:
  const std::string prefix_;
};

bool ParseJson(const std::string& text, Json::Value* value) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errors;
  return reader->parse(text.data(), text.data() + text.size(), value,
                       &errors);
}

class LocalDescriptionObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit LocalDescriptionObserver(TigasSender* sender) : sender_(sender) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    sender_->OnLocalDescriptionSet(std::move(error));
  }

 private:
  TigasSender* const sender_;
};

class RemoteDescriptionObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit RemoteDescriptionObserver(TigasSender* sender) : sender_(sender) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    sender_->OnRemoteDescriptionSet(std::move(error));
  }

 private:
  TigasSender* const sender_;
};

class StatsCallback : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsCallback(TigasSender* sender) : sender_(sender) {}
  void OnStatsDelivered(
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    sender_->OnStatsDelivered(report);
  }

 private:
  TigasSender* const sender_;
};

}  // namespace

std::atomic<bool> TigasSender::quit_requested_{false};

TigasSender::TigasSender(PeerConnectionClient* client,
                         const SenderOptions& options)
    : client_(client),
      options_(options),
      main_thread_(rtc::Thread::Current()) {
  client_->RegisterObserver(this);
}

TigasSender::~TigasSender() {
  if (frames_log_)
    fclose(frames_log_);
  if (stats_log_)
    fclose(stats_log_);
}

void TigasSender::RequestQuit() {
  quit_requested_ = true;
}

bool TigasSender::Start() {
  start_ms_ = rtc::TimeMillis();
  const std::string dir = options_.out_dir.empty() ? "." : options_.out_dir;
  frames_log_ = fopen((dir + "/sender_frames.ndjson").c_str(), "w");
  stats_log_ = fopen((dir + "/sender_stats.ndjson").c_str(), "w");
  if (!frames_log_ || !stats_log_) {
    RTC_LOG(LS_ERROR) << "Cannot open the logs in " << dir;
    return false;
  }

  if (!render_.Connect(options_.render_socket,
                       options_.render_connect_timeout_s))
    return false;

  // No audio track; the fake module keeps the factory away from the sound
  // devices.
  static webrtc::FakeAudioDeviceModule adm;
  // The worker thread is created by the factory. The main thread is the
  // signaling thread, so the signaling client, the PeerConnection callbacks
  // and the DataChannel messages all run on it.
  network_thread_ = rtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("tigas_network", nullptr);
  network_thread_->Start();
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(), nullptr /* worker_thread */, main_thread_,
      rtc::scoped_refptr<webrtc::AudioDeviceModule>(&adm),
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      // In sparkrtc the OpenH264 adapter creates the x264 encoder.
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::OpenH264EncoderTemplateAdapter>>(),
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::OpenH264DecoderTemplateAdapter>>(),
      nullptr /* audio_mixer */, nullptr /* audio_processing */);
  if (!factory_) {
    RTC_LOG(LS_ERROR) << "Failed to create the PeerConnectionFactory";
    return false;
  }

  render_thread_ = std::thread([this] { RenderLoop(); });
  main_thread_->PostTask([this] { Tick(); });
  client_->Connect(options_.server, options_.port, options_.name);
  return true;
}

bool TigasSender::CreatePeerConnection() {
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // No STUN or TURN server: the page and the sender sit on the two sides of
  // rattan, and a STUN lookup would only wait for a timeout.
  config.tcp_candidate_policy =
      webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  config.bundle_policy =
      webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy =
      webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

  webrtc::PeerConnectionDependencies deps(this);
  if (!options_.candidate_prefix.empty()) {
    network_thread_->BlockingCall([this] {
      network_manager_ = std::make_unique<PrefixNetworkManager>(
          network_thread_->socketserver(), options_.candidate_prefix);
      socket_factory_ = std::make_unique<rtc::BasicPacketSocketFactory>(
          network_thread_->socketserver());
    });
    // As PeerConnectionFactory builds the default allocator.
    deps.allocator = std::make_unique<cricket::BasicPortAllocator>(
        network_manager_.get(), socket_factory_.get());
    deps.allocator->SetPortRange(config.port_allocator_config.min_port,
                                 config.port_allocator_config.max_port);
    deps.allocator->set_flags(config.port_allocator_config.flags);
  }
  auto pc_or_error =
      factory_->CreatePeerConnectionOrError(config, std::move(deps));
  if (!pc_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                      << pc_or_error.error().message();
    return false;
  }
  pc_ = pc_or_error.MoveValue();

  source_ = rtc::make_ref_counted<PushVideoSource>();
  rtc::scoped_refptr<webrtc::VideoTrackInterface> track =
      factory_->CreateVideoTrack(source_, "tigas_video");
  auto sender_or_error = pc_->AddTrack(track, {"tigas"});
  if (!sender_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "AddTrack failed: "
                      << sender_or_error.error().message();
    return false;
  }
  webrtc::RtpParameters params = sender_or_error.value()->GetParameters();
  RTC_LOG(LS_INFO) << "Video sender degradation preference: "
                   << (params.degradation_preference
                           ? webrtc::DegradationPreferenceToString(
                                 *params.degradation_preference)
                           : "default (screencast: maintain-resolution)");

  // Same parameters on the page: ordered, reliable, negotiated, id 0.
  webrtc::DataChannelInit init;
  init.ordered = true;
  init.negotiated = true;
  init.id = 0;
  auto dc_or_error = pc_->CreateDataChannelOrError("tigas", &init);
  if (!dc_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "CreateDataChannel failed: "
                      << dc_or_error.error().message();
    return false;
  }
  dc_ = dc_or_error.MoveValue();
  dc_->RegisterObserver(this);
  return true;
}

// ---------------------------------------------------------------------------
// Signaling.

void TigasSender::OnSignedIn() {
  RTC_LOG(LS_INFO) << "Signed in to the signaling server as " << options_.name;
}

void TigasSender::OnDisconnected() {
  RTC_LOG(LS_INFO) << "Signed out of the signaling server";
}

void TigasSender::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(LS_INFO) << "Peer " << id << " (" << name << ") signed in";
  if (peer_id_ != -1 || shutting_down_)
    return;
  // The first page that signs in is the receiver; the sender makes the offer.
  peer_id_ = id;
  if (!CreatePeerConnection()) {
    Shutdown("cannot create the PeerConnection");
    return;
  }
  pc_->SetLocalDescription(
      rtc::make_ref_counted<LocalDescriptionObserver>(this));
}

void TigasSender::OnPeerDisconnected(int peer_id) {
  RTC_LOG(LS_INFO) << "Peer " << peer_id << " left";
  if (peer_id == peer_id_)
    Shutdown("peer left");
}

void TigasSender::OnMessageFromPeer(int peer_id, const std::string& message) {
  if (peer_id != peer_id_ || !pc_) {
    RTC_LOG(LS_WARNING) << "Ignoring a message from peer " << peer_id;
    return;
  }
  Json::Value json;
  if (!ParseJson(message, &json) || !json.isObject()) {
    RTC_LOG(LS_WARNING) << "Bad signaling message: " << message;
    return;
  }
  if (json.isMember("type")) {
    const std::string type = json["type"].asString();
    if (type != "answer") {
      RTC_LOG(LS_WARNING) << "Unexpected session description type " << type;
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer,
                                         json["sdp"].asString(), &error);
    if (!desc) {
      RTC_LOG(LS_ERROR) << "Cannot parse the answer: " << error.description;
      Shutdown("bad answer");
      return;
    }
    RTC_LOG(LS_INFO) << "Received the answer";
    pc_->SetRemoteDescription(
        std::move(desc), rtc::make_ref_counted<RemoteDescriptionObserver>(this));
    return;
  }

  const std::string sdp = json["candidate"].asString();
  if (sdp.empty())
    return;  // End of candidates.
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> candidate(
      webrtc::CreateIceCandidate(json["sdpMid"].asString(),
                                 json["sdpMLineIndex"].asInt(), sdp, &error));
  if (!candidate) {
    RTC_LOG(LS_WARNING) << "Cannot parse a remote candidate: "
                        << error.description;
    return;
  }
  RTC_LOG(LS_INFO) << "Remote candidate " << sdp;
  if (!remote_description_set_) {
    pending_remote_candidates_.push_back(std::move(candidate));
    return;
  }
  pc_->AddIceCandidate(std::move(candidate), [](webrtc::RTCError error) {
    if (!error.ok())
      RTC_LOG(LS_WARNING) << "AddIceCandidate failed: " << error.message();
  });
}

void TigasSender::OnMessageSent(int err) {
  SendNextSignal();
}

void TigasSender::OnServerConnectionFailure() {
  Shutdown("cannot reach the signaling server");
}

void TigasSender::SendSignal(const std::string& message) {
  pending_signals_.push_back(message);
  SendNextSignal();
}

void TigasSender::SendNextSignal() {
  // The signaling client sends one message at a time.
  if (pending_signals_.empty() || peer_id_ < 0 || client_->IsSendingMessage())
    return;
  if (!client_->SendToPeer(peer_id_, pending_signals_.front()))
    RTC_LOG(LS_ERROR) << "SendToPeer failed";
  pending_signals_.pop_front();
}

void TigasSender::OnLocalDescriptionSet(webrtc::RTCError error) {
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "SetLocalDescription failed: " << error.message();
    Shutdown("SetLocalDescription failed");
    return;
  }
  const webrtc::SessionDescriptionInterface* desc = pc_->local_description();
  std::string sdp;
  desc->ToString(&sdp);
  Json::Value json;
  json["type"] = webrtc::SdpTypeToString(desc->GetType());
  json["sdp"] = sdp;
  RTC_LOG(LS_INFO) << "Sending the offer";
  SendSignal(ToJsonString(json));
}

void TigasSender::OnRemoteDescriptionSet(webrtc::RTCError error) {
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << error.message();
    Shutdown("SetRemoteDescription failed");
    return;
  }
  remote_description_set_ = true;
  for (auto& candidate : pending_remote_candidates_) {
    pc_->AddIceCandidate(std::move(candidate), [](webrtc::RTCError error) {
      if (!error.ok())
        RTC_LOG(LS_WARNING) << "AddIceCandidate failed: " << error.message();
    });
  }
  pending_remote_candidates_.clear();
}

// ---------------------------------------------------------------------------
// PeerConnectionObserver.

void TigasSender::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  RTC_LOG(LS_INFO) << "Signaling state "
                   << webrtc::PeerConnectionInterface::AsString(new_state);
}

void TigasSender::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  RTC_LOG(LS_WARNING) << "Unexpected in-band data channel "
                      << channel->label();
}

void TigasSender::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  RTC_LOG(LS_INFO) << "ICE gathering state "
                   << webrtc::PeerConnectionInterface::AsString(new_state);
}

void TigasSender::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  std::string sdp;
  if (!candidate->ToString(&sdp))
    return;
  const std::string address =
      candidate->candidate().address().ipaddr().ToString();
  const bool keep = options_.candidate_prefix.empty() ||
                    absl::StartsWith(address, options_.candidate_prefix);
  RTC_LOG(LS_INFO) << "Local candidate " << sdp
                   << (keep ? " (sent)" : " (dropped)");
  if (!keep)
    return;
  Json::Value json;
  json["sdpMid"] = candidate->sdp_mid();
  json["sdpMLineIndex"] = candidate->sdp_mline_index();
  json["candidate"] = sdp;
  SendSignal(ToJsonString(json));
}

void TigasSender::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  RTC_LOG(LS_INFO) << "Connection state "
                   << webrtc::PeerConnectionInterface::AsString(new_state);
  if (new_state ==
      webrtc::PeerConnectionInterface::PeerConnectionState::kFailed)
    Shutdown("connection failed");
}

void TigasSender::OnIceSelectedCandidatePairChanged(
    const cricket::CandidatePairChangeEvent& event) {
  RTC_LOG(LS_INFO)
      << "Selected candidate pair "
      << event.selected_candidate_pair.local_candidate().address().ToString()
      << " -> "
      << event.selected_candidate_pair.remote_candidate().address().ToString();
}

// ---------------------------------------------------------------------------
// DataChannel.

void TigasSender::OnStateChange() {
  const webrtc::DataChannelInterface::DataState state = dc_->state();
  RTC_LOG(LS_INFO) << "Data channel "
                   << webrtc::DataChannelInterface::DataStateString(state);
  if (state == webrtc::DataChannelInterface::kClosed)
    Shutdown("data channel closed");
}

void TigasSender::OnMessage(const webrtc::DataBuffer& buffer) {
  Pose pose;
  pose.t_arrive = WallMs();
  pose.t_arrive_mono = MonoMs();
  pose.line.assign(buffer.data.data<char>(), buffer.data.size());
  Json::Value json;
  if (!ParseJson(pose.line, &json) || !json.isObject()) {
    RTC_LOG(LS_WARNING) << "Bad data channel message: " << pose.line;
    return;
  }
  if (json.isMember("type") && json["type"].asString() == "bye") {
    Shutdown("page finished");
    return;
  }
  if (!json.isMember("i") || !json.isMember("body")) {
    RTC_LOG(LS_WARNING) << "Data channel message without i or body";
    return;
  }
  pose.i = json["i"].asInt();
  if (json.isMember("s"))
    pose.seq = json["s"].asInt64();
  // The line has no newline in it: the page sends JSON.stringify output.
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (options_.push_fps > 0) {
      latest_ = std::move(pose);
      has_latest_ = true;
      since_tick_++;
    } else {
      pose.queue_ahead = queue_.size();
      queue_.push_back(std::move(pose));
    }
  }
  cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Render thread: one viewpoint at a time, in arrival order.

void TigasSender::RenderLoop() {
  if (options_.push_fps > 0) {
    PushLoop();
    return;
  }
  while (true) {
    Pose pose;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      pose = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!RenderAndPush(pose, -1))
      return;
  }
}

// Push mode: tick k renders the latest pose at start + k / push_fps, or as
// soon as the render of tick k - 1 is done if that is later; late ticks are
// not made up for. The pose is rendered again even if it has not changed.
// Ticks start with the first pose.
void TigasSender::PushLoop() {
  const double period_ms = 1000.0 / options_.push_fps;
  double due_ms = 0;
  for (int64_t tick = 0;; tick++) {
    Pose pose;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (tick == 0) {
        cv_.wait(lock, [this] { return stop_ || has_latest_; });
        due_ms = MonoMs();
      } else {
        due_ms = std::max(due_ms + period_ms, MonoMs());
        cv_.wait_for(lock,
                     std::chrono::microseconds(
                         static_cast<int64_t>((due_ms - MonoMs()) * 1000)),
                     [this] { return stop_; });
      }
      if (stop_)
        return;
      pose = latest_;
      pose.queue_ahead = since_tick_;
      since_tick_ = 0;
    }
    if (!RenderAndPush(pose, tick))
      return;
  }
}

bool TigasSender::RenderAndPush(const Pose& pose, int64_t tick) {
  const double t_render_start = WallMs();
  RenderReply reply;
  if (!render_.Render(pose.line, &reply)) {
    main_thread_->PostTask([this] { Shutdown("render worker failed"); });
    return false;
  }
  const double t_render_end = WallMs();
  if (!reply.buffer) {
    RTC_LOG(LS_ERROR) << "The worker could not render viewpoint " << pose.i;
    fprintf(frames_log_, "{\"i\":%d,\"error\":\"render\",\"t_arrive\":%.3f}\n",
            pose.i, pose.t_arrive);
    fflush(frames_log_);
    return true;
  }
  if (reply.i != pose.i)
    RTC_LOG(LS_WARNING) << "Worker replied for viewpoint " << reply.i
                        << " instead of " << pose.i;

  // The capture time must increase, or the encoder drops the frame
  // (video/video_stream_encoder.cc). With the field trial
  // WebRTC-Tigas-ZeroRtpTimestampOffset the wire RTP timestamp is
  // 90 * ntp_ms mod 2^32.
  int64_t ntp_ms = rtc::TimeUTCMillis() + kNtpJan1970Ms;
  if (ntp_ms <= last_ntp_ms_)
    ntp_ms = last_ntp_ms_ + 1;
  last_ntp_ms_ = ntp_ms;
  const uint32_t rtp_ts = 90u * static_cast<uint32_t>(ntp_ms);

  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(reply.buffer)
                                 .set_timestamp_us(rtc::TimeMicros())
                                 .set_ntp_time_ms(ntp_ms)
                                 .set_rotation(webrtc::kVideoRotation_0)
                                 .build();
  const double t_push = WallMs();
  const double t_push_mono = MonoMs();
  source_->Push(frame);

  // Push mode adds the tick and the number of the page's message.
  char push[64] = "";
  if (tick >= 0)
    snprintf(push, sizeof(push), ",\"tick\":%" PRId64 ",\"s\":%" PRId64,
             tick, pose.seq);
  char meta[224];
  snprintf(meta, sizeof(meta),
           "{\"i\":%d,\"rtp_ts\":%u,\"render_ms\":%.3f,\"t_push\":%.3f%s}",
           pose.i, rtp_ts, reply.render_ms, t_push, push);
  const bool meta_sent = dc_->Send(webrtc::DataBuffer(std::string(meta)));

  fprintf(frames_log_,
          "{\"i\":%d,\"t_arrive\":%.3f,\"t_arrive_mono\":%.3f,"
          "\"queue_ahead\":%zu,\"t_render_start\":%.3f,"
          "\"t_render_end\":%.3f,\"render_ms\":%.3f,\"ntp_ms\":%" PRId64
          ",\"rtp_ts\":%u,\"t_push\":%.3f,\"t_push_mono\":%.3f,"
          "\"meta_sent\":%s%s}\n",
          pose.i, pose.t_arrive, pose.t_arrive_mono, pose.queue_ahead,
          t_render_start, t_render_end, reply.render_ms, ntp_ms, rtp_ts,
          t_push, t_push_mono, meta_sent ? "true" : "false", push);
  fflush(frames_log_);
  return true;
}

// ---------------------------------------------------------------------------
// Stats and shutdown (main thread).

void TigasSender::Tick() {
  if (shutting_down_)
    return;
  if (quit_requested_) {
    Shutdown("signal");
    return;
  }
  if (rtc::TimeMillis() - start_ms_ > options_.max_duration_s * 1000LL) {
    Shutdown("max duration");
    return;
  }
  if (pc_)
    pc_->GetStats(rtc::make_ref_counted<StatsCallback>(this).get());
  main_thread_->PostDelayedTask(
      [this] { Tick(); }, webrtc::TimeDelta::Millis(options_.stats_interval_ms));
}

void TigasSender::OnStatsDelivered(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  if (shutting_down_)
    return;
  std::string pair_id;
  for (const webrtc::RTCTransportStats* transport :
       report->GetStatsOfType<webrtc::RTCTransportStats>()) {
    if (transport->selected_candidate_pair_id.is_defined())
      pair_id = *transport->selected_candidate_pair_id;
  }
  std::string local_id, remote_id;
  if (const webrtc::RTCStats* stats = report->Get(pair_id)) {
    const auto& pair = stats->cast_to<webrtc::RTCIceCandidatePairStats>();
    local_id = pair.local_candidate_id.ValueOrDefault("");
    remote_id = pair.remote_candidate_id.ValueOrDefault("");
  }
  if (pair_id != selected_pair_) {
    selected_pair_ = pair_id;
    RTC_LOG(LS_INFO) << "Stats: selected pair " << pair_id << ", local "
                     << local_id << ", remote " << remote_id;
  }

  char head[128];
  snprintf(head, sizeof(head), "{\"t_wall\":%.3f,\"t_mono\":%.3f,\"stats\":[",
           WallMs(), MonoMs());
  std::string line = head;
  bool first = true;
  for (const webrtc::RTCStats& stats : *report) {
    const char* type = stats.type();
    const bool keep = strcmp(type, "outbound-rtp") == 0 ||
                      strcmp(type, "remote-inbound-rtp") == 0 ||
                      strcmp(type, "media-source") == 0 ||
                      strcmp(type, "transport") == 0 ||
                      strcmp(type, "data-channel") == 0 ||
                      (!pair_id.empty() && stats.id() == pair_id) ||
                      (!local_id.empty() && stats.id() == local_id) ||
                      (!remote_id.empty() && stats.id() == remote_id);
    if (!keep)
      continue;
    if (!first)
      line += ",";
    first = false;
    line += stats.ToJson();
  }
  line += "]}\n";
  fputs(line.c_str(), stats_log_);
  fflush(stats_log_);
}

void TigasSender::Shutdown(const char* reason) {
  if (shutting_down_)
    return;
  shutting_down_ = true;
  RTC_LOG(LS_INFO) << "Shutting down: " << reason;

  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  // Unblocks a render in progress; the worker then waits for the next
  // sender.
  render_.Shutdown();
  if (render_thread_.joinable())
    render_thread_.join();

  if (dc_) {
    dc_->UnregisterObserver();
    dc_->Close();
    dc_ = nullptr;
  }
  if (pc_) {
    pc_->Close();
    pc_ = nullptr;
  }
  source_ = nullptr;
  factory_ = nullptr;
  if (network_manager_) {
    network_thread_->BlockingCall([this] {
      network_manager_.reset();
      socket_factory_.reset();
    });
  }
  client_->SignOut();
  main_thread_->Quit();
}

}  // namespace tigas
