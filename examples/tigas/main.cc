/*
 *  TIGAS sender. Signs in to peerconnection_server, waits for the page,
 *  offers one video track and one negotiated DataChannel, and renders every
 *  viewpoint the page sends through tigas/webrtc/render_worker.py.
 */

#include <signal.h>
#include <stdio.h>

#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "examples/peerconnection/localvideo/peer_connection_localvideo.h"
#include "examples/tigas/tigas_sender.h"
#include "rtc_base/helpers.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"

ABSL_FLAG(std::string, server, "127.0.0.1", "Signaling server address.");
ABSL_FLAG(int, port, 8888, "Signaling server port.");
ABSL_FLAG(std::string, name, "tigas_sender", "Name used on the server.");
ABSL_FLAG(std::string,
          render_socket,
          "/tmp/tigas_render.sock",
          "Unix socket of the render worker.");
ABSL_FLAG(int,
          render_connect_timeout_s,
          120,
          "How long to wait for the render worker to listen.");
ABSL_FLAG(std::string,
          out_dir,
          ".",
          "Directory for sender_info.json, sender_frames.ndjson and "
          "sender_stats.ndjson.");
ABSL_FLAG(std::string,
          candidate_prefix,
          "10.2.",
          "Only send local ICE candidates whose address starts with this; "
          "empty sends all.");
ABSL_FLAG(int, stats_interval_ms, 100, "GetStats period.");
ABSL_FLAG(int, max_duration_s, 900, "Exit after this long in any case.");
// The three sparkrtc switches (rtc_base/helpers.h). MAE is off by default
// here; the localvideo example turns it on.
ABSL_FLAG(bool, enable_mae, false, "Enable the More Adaptive Encoder.");
ABSL_FLAG(bool,
          enable_adaptive_frame_drop,
          false,
          "Re-enable frame dropping under sustained congestion or encoder "
          "overuse.");
ABSL_FLAG(bool, disable_frame_drop, true, "Disable frame dropping.");
ABSL_FLAG(std::string,
          force_fieldtrials,
          "WebRTC-Bwe-StableBandwidthEstimate/Enabled/"
          "WebRTC-Bwe-ProbeRateFallback/Enabled/"
          "WebRTC-AddPacingToCongestionWindowPushback/Enabled/"
          "WebRTC-Tigas-ZeroRtpTimestampOffset/Enabled/"
          "WebRTC-Tigas-X264KeyFrameFix/Enabled/"
          "WebRTC-Bwe-RobustThroughputEstimatorSettings/"
          "window_packets:20,max_window_packets:500,window_duration:1s,"
          "max_window_duration:5s/",
          "Field trials. The first three are the localvideo defaults. "
          "ZeroRtpTimestampOffset makes the wire RTP timestamp 90 * capture "
          "NTP ms; X264KeyFrameFix makes x264 answer key frame requests "
          "(modules/video_coding/codecs/h264/x264_encoder_impl.h). "
          "RobustThroughputEstimatorSettings puts back the upstream window "
          "of the acknowledged bitrate: sparkrtc's 20-packet cap with "
          "pacing factor 100 can make GCC back off to 0.85 * the send rate "
          "of two frames' bursts (acknowledged_bitrate_estimator_interface.h). "
          "Add WebRTC-Tigas-EncodeEveryFrame/Enabled/ to encode every queued "
          "frame instead of only the newest (video/video_stream_encoder.h).");

namespace {

void OnSignal(int) {
  tigas::TigasSender::RequestQuit();
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

bool WriteInfo(const std::string& dir, const std::string& field_trials) {
  FILE* f = fopen((dir + "/sender_info.json").c_str(), "w");
  if (!f)
    return false;
  fprintf(f,
          "{\"enable_mae\":%s,\"enable_adaptive_frame_drop\":%s,"
          "\"disable_frame_drop\":%s,\"field_trials\":\"%s\","
          "\"candidate_prefix\":\"%s\",\"stats_interval_ms\":%d}\n",
          rtc::GetEnableMAE() ? "true" : "false",
          rtc::GetEnableAdaptiveFrameDrop() ? "true" : "false",
          rtc::GetDisableFrameDrop() ? "true" : "false",
          JsonEscape(field_trials).c_str(),
          JsonEscape(absl::GetFlag(FLAGS_candidate_prefix)).c_str(),
          absl::GetFlag(FLAGS_stats_interval_ms));
  fclose(f);
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  rtc::LogMessage::LogTimestamps(true);
  rtc::LogMessage::LogThreads(true);
  rtc::LogMessage::LogToDebug(rtc::LS_INFO);
  rtc::LogMessage::SetLogToStderr(true);

  // InitFieldTrialsFromString keeps the pointer, so the string must outlive
  // the program.
  static const std::string field_trials =
      absl::GetFlag(FLAGS_force_fieldtrials);
  webrtc::field_trial::InitFieldTrialsFromString(field_trials.c_str());

  rtc::SetEnableMAE(absl::GetFlag(FLAGS_enable_mae));
  rtc::SetEnableAdaptiveFrameDrop(
      absl::GetFlag(FLAGS_enable_adaptive_frame_drop));
  rtc::SetDisableFrameDrop(absl::GetFlag(FLAGS_disable_frame_drop));

  tigas::SenderOptions options;
  options.server = absl::GetFlag(FLAGS_server);
  options.port = absl::GetFlag(FLAGS_port);
  options.name = absl::GetFlag(FLAGS_name);
  options.render_socket = absl::GetFlag(FLAGS_render_socket);
  options.render_connect_timeout_s =
      absl::GetFlag(FLAGS_render_connect_timeout_s);
  options.out_dir = absl::GetFlag(FLAGS_out_dir);
  options.candidate_prefix = absl::GetFlag(FLAGS_candidate_prefix);
  options.stats_interval_ms = absl::GetFlag(FLAGS_stats_interval_ms);
  options.max_duration_s = absl::GetFlag(FLAGS_max_duration_s);

  if (!WriteInfo(options.out_dir, field_trials)) {
    fprintf(stderr, "Cannot write %s/sender_info.json\n",
            options.out_dir.c_str());
    return 1;
  }

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  // A plain socket server: it blocks until the next socket event or task, so
  // nothing on the main thread waits for a polling interval.
  rtc::PhysicalSocketServer socket_server;
  rtc::AutoSocketServerThread main_thread(&socket_server);
  rtc::InitializeSSL();

  int rc = 0;
  {
    // Must be constructed after the socket server is set.
    PeerConnectionClient client;
    tigas::TigasSender sender(&client, options);
    if (sender.Start())
      main_thread.Run();
    else
      rc = 1;
  }
  rtc::CleanupSSL();
  return rc;
}
