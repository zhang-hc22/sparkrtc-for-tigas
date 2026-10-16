/*
 *  TIGAS sender: renders each viewpoint the page sends over the DataChannel
 *  and streams the frames as one video track (x264, GCC).
 */

#ifndef EXAMPLES_TIGAS_TIGAS_SENDER_H_
#define EXAMPLES_TIGAS_TIGAS_SENDER_H_

#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "examples/peerconnection/localvideo/peer_connection_localvideo.h"
#include "examples/tigas/push_video_source.h"
#include "examples/tigas/render_client.h"
#include "rtc_base/thread.h"

namespace tigas {

struct SenderOptions {
  std::string server;
  int port = 8888;
  std::string name;
  std::string render_socket;
  int render_connect_timeout_s = 120;
  std::string out_dir;
  // Only local ICE candidates whose address starts with this are sent to the
  // page. Empty sends all.
  std::string candidate_prefix;
  int stats_interval_ms = 100;
  int max_duration_s = 900;
};

class TigasSender : public webrtc::PeerConnectionObserver,
                    public webrtc::DataChannelObserver,
                    public PeerConnectionClientObserver {
 public:
  TigasSender(PeerConnectionClient* client, const SenderOptions& options);
  ~TigasSender() override;

  // Called on the main (signaling) thread before it runs.
  bool Start();
  // Asks the main thread to shut down; safe from a signal handler.
  static void RequestQuit();

  // Called back from the observer adapters in tigas_sender.cc.
  void OnLocalDescriptionSet(webrtc::RTCError error);
  void OnRemoteDescriptionSet(webrtc::RTCError error);
  void OnStatsDelivered(
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

 private:
  struct Pose {
    int i = 0;
    std::string line;
    double t_arrive = 0;       // wall clock, ms
    double t_arrive_mono = 0;  // monotonic clock, ms
    size_t queue_ahead = 0;    // poses waiting when this one arrived
  };

  // PeerConnectionObserver.
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
  void OnIceSelectedCandidatePairChanged(
      const cricket::CandidatePairChangeEvent& event) override;

  // DataChannelObserver.
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;

  // PeerConnectionClientObserver.
  void OnSignedIn() override;
  void OnDisconnected() override;
  void OnPeerConnected(int id, const std::string& name) override;
  void OnPeerDisconnected(int peer_id) override;
  void OnMessageFromPeer(int peer_id, const std::string& message) override;
  void OnMessageSent(int err) override;
  void OnServerConnectionFailure() override;

  bool CreatePeerConnection();
  void SendSignal(const std::string& message);
  void SendNextSignal();
  void Tick();
  void RenderLoop();
  void Shutdown(const char* reason);

  PeerConnectionClient* const client_;
  const SenderOptions options_;
  rtc::Thread* const main_thread_;

  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  rtc::scoped_refptr<PushVideoSource> source_;
  // Written on the main thread before the render thread starts; read there.
  rtc::scoped_refptr<webrtc::DataChannelInterface> dc_;

  int peer_id_ = -1;
  std::deque<std::string> pending_signals_;
  // Page candidates that arrive before the answer has been applied.
  std::vector<std::unique_ptr<webrtc::IceCandidateInterface>>
      pending_remote_candidates_;
  bool remote_description_set_ = false;
  bool shutting_down_ = false;
  int64_t start_ms_ = 0;
  std::string selected_pair_;

  RenderClient render_;
  std::thread render_thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Pose> queue_;  // guarded by mu_
  bool stop_ = false;       // guarded by mu_
  int64_t last_ntp_ms_ = 0;  // render thread only

  FILE* frames_log_ = nullptr;  // render thread only after Start()
  FILE* stats_log_ = nullptr;   // main thread only

  static std::atomic<bool> quit_requested_;
};

}  // namespace tigas

#endif  // EXAMPLES_TIGAS_TIGAS_SENDER_H_
