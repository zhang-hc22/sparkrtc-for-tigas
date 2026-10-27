/*
 *  TIGAS sender. Signs in to peerconnection_server, waits for the page,
 *  offers one video track and one negotiated DataChannel, and renders every
 *  viewpoint the page sends through tigas/webrtc/render_worker.py, or with
 *  --push_fps the latest viewpoint at a fixed rate.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <initializer_list>
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
ABSL_FLAG(int,
          push_fps,
          0,
          "0: one frame per viewpoint message. Above 0: render the latest "
          "viewpoint this many times a second, also when it has not "
          "changed, as the QUIC server does for /player-wt.");
// The three sparkrtc switches (rtc_base/helpers.h). MAE is off by default
// here; the localvideo example turns it on.
ABSL_FLAG(bool, enable_mae, false, "Enable the More Adaptive Encoder.");
ABSL_FLAG(bool,
          enable_salsify,
          false,
          "Enable the MAE repository's codec-3 Salsify baseline: x264 VBV "
          "fixed at 0.04 seconds after SetRates. Independent of MAE and "
          "Mortise; not with --enable_mae. With --mortise, Mortise sets "
          "GCC's target and Salsify keeps its VBV.");
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
ABSL_FLAG(double,
          gcc_ai,
          1.0,
          "Scale of GCC's additive increase "
          "(modules/remote_bitrate_estimator/aimd_rate_control.cc).");
ABSL_FLAG(double,
          gcc_mi,
          1.08,
          "Per-second factor of GCC's multiplicative increase. If this or "
          "--gcc_ai is not the default, WebRTC-Tigas-AimdIncrease/ai:..,mi:../ "
          "is added to the field trials.");
ABSL_FLAG(bool,
          enable_mortise,
          false,
          "Let Mortise set GCC's back-off factor, multiplicative and additive "
          "increase about once a second "
          "(modules/remote_bitrate_estimator/tradeoff_control.h): adds "
          "WebRTC-Tigas-Mortise/Enabled/ to the field trials. Not with "
          "--gcc_ai or --gcc_mi. The same as --mortise=v1.");
ABSL_FLAG(std::string,
          mortise,
          "",
          "Mortise's version: v1 is the controller of 920865ec29 "
          "(mortise_v1), as --enable_mortise; v2 is v1 with the options "
          "umin_rq:1,probe:0.1,det_overuse_ms:100 (mortise_v2), the same "
          "field trial as --mortise=v1 --mortise_args=... with them; v3 is "
          "the controller with the structure of Mortise's Copa controller, "
          "v3:1,umin_rq:1,det_overuse_ms:100 (mortise_v3). Empty: off.");
ABSL_FLAG(std::string,
          mortise_args,
          "",
          "Experiment options on top of --mortise=v1, key:value,... with keys "
          "fixed_u (0 to 1), umin_rq (0 or 1), probe (a step of u), "
          "det_overuse_ms and det_window_ms (GCC's overuse detector for "
          "u <= 0.8, its default at u = 1), v3 (0 or 1), v3_hist, v3_sigma, "
          "v3_dither, v3_fine (0 or 1), v3_est (0 or 1) and umin_hr (0 or 1) "
          "(modules/remote_bitrate_estimator/tradeoff_control.h): the field "
          "trial becomes WebRTC-Tigas-Mortise/Enabled,ARGS/.");
ABSL_FLAG(std::string,
          gcc_detector_args,
          "",
          "GCC's overuse detector, key:value,... with keys window_ms (time "
          "window of the slope fit, default 100), overuse_ms (how long an "
          "overuse must last, 10), gain (of the slope, 4) and underuse_hold "
          "(0 takes an underuse as normal, 1): adds "
          "WebRTC-Tigas-Detector/ARGS/ "
          "(modules/congestion_controller/goog_cc/trendline_estimator.h, "
          "modules/remote_bitrate_estimator/aimd_rate_control.h).");

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

// ARGS is empty or key:value,... with each key in keys; the keys in flags
// take 0 or 1, the others a number.
bool CheckArgs(const char* flag,
               const std::string& args,
               std::initializer_list<const char*> keys,
               std::initializer_list<const char*> flags) {
  size_t start = 0;
  while (start < args.size()) {
    size_t end = args.find(',', start);
    if (end == std::string::npos)
      end = args.size();
    const std::string item = args.substr(start, end - start);
    const size_t colon = item.find(':');
    const std::string key = item.substr(0, colon);
    const std::string value =
        colon == std::string::npos ? "" : item.substr(colon + 1);
    bool known = false, is_flag = false;
    for (const char* k : keys)
      known |= key == k;
    for (const char* k : flags)
      is_flag |= key == k;
    char* rest = nullptr;
    strtod(value.c_str(), &rest);
    const bool ok = is_flag ? (value == "0" || value == "1")
                            : (!value.empty() && *rest == '\0');
    if (!known || !ok) {
      fprintf(stderr, "--%s: bad item '%s'\n", flag, item.c_str());
      return false;
    }
    start = end + 1;
  }
  return true;
}

bool WriteInfo(const std::string& dir,
               const std::string& field_trials,
               const std::string& mortise) {
  FILE* f = fopen((dir + "/sender_info.json").c_str(), "w");
  if (!f)
    return false;
  fprintf(f,
          "{\"enable_mae\":%s,\"enable_adaptive_frame_drop\":%s,"
          "\"disable_frame_drop\":%s,\"field_trials\":\"%s\","
          "\"candidate_prefix\":\"%s\",\"stats_interval_ms\":%d,"
          "\"push_fps\":%d,\"enable_mortise\":%s,\"mortise\":\"%s\","
          "\"mortise_args\":\"%s\",\"gcc_detector_args\":\"%s\","
          "\"enable_salsify\":%s}\n",
          rtc::GetEnableMAE() ? "true" : "false",
          rtc::GetEnableAdaptiveFrameDrop() ? "true" : "false",
          rtc::GetDisableFrameDrop() ? "true" : "false",
          JsonEscape(field_trials).c_str(),
          JsonEscape(absl::GetFlag(FLAGS_candidate_prefix)).c_str(),
          absl::GetFlag(FLAGS_stats_interval_ms),
          absl::GetFlag(FLAGS_push_fps), mortise.empty() ? "false" : "true",
          JsonEscape(mortise).c_str(),
          JsonEscape(absl::GetFlag(FLAGS_mortise_args)).c_str(),
          JsonEscape(absl::GetFlag(FLAGS_gcc_detector_args)).c_str(),
          webrtc::field_trial::IsEnabled("WebRTC-Tigas-Salsify") ? "true"
                                                              : "false");
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
  static std::string field_trials = absl::GetFlag(FLAGS_force_fieldtrials);
  const double gcc_ai = absl::GetFlag(FLAGS_gcc_ai);
  const double gcc_mi = absl::GetFlag(FLAGS_gcc_mi);
  if (gcc_ai != 1.0 || gcc_mi != 1.08) {
    char trial[96];
    snprintf(trial, sizeof(trial), "WebRTC-Tigas-AimdIncrease/ai:%g,mi:%g/",
             gcc_ai, gcc_mi);
    field_trials += trial;
  }
  std::string mortise = absl::GetFlag(FLAGS_mortise);
  if (absl::GetFlag(FLAGS_enable_mortise)) {
    if (!mortise.empty() && mortise != "v1") {
      fprintf(stderr, "--enable_mortise is --mortise=v1\n");
      return 1;
    }
    mortise = "v1";
  }
  if (!mortise.empty() && mortise != "v1" && mortise != "v2" &&
      mortise != "v3") {
    fprintf(stderr, "--mortise: unknown version '%s'\n", mortise.c_str());
    return 1;
  }
  std::string mortise_args = absl::GetFlag(FLAGS_mortise_args);
  if (!mortise_args.empty() && mortise.empty()) {
    fprintf(stderr, "--mortise_args needs --mortise\n");
    return 1;
  }
  if (mortise == "v2" || mortise == "v3") {
    if (!mortise_args.empty()) {
      fprintf(stderr,
              "--mortise=%s has its options; for others use --mortise=v1 "
              "--mortise_args=...\n",
              mortise.c_str());
      return 1;
    }
    mortise_args = mortise == "v2" ? "umin_rq:1,probe:0.1,det_overuse_ms:100"
                                   : "v3:1,umin_rq:1,det_overuse_ms:100";
  }
  if (!CheckArgs("mortise_args", mortise_args,
                 {"fixed_u", "umin_rq", "probe", "det_overuse_ms",
                  "det_window_ms", "v3", "v3_hist", "v3_sigma", "v3_dither",
                  "v3_fine", "v3_est", "umin_hr"},
                 {"umin_rq", "v3", "v3_fine", "umin_hr"}))
    return 1;
  if (!mortise.empty()) {
    if (gcc_ai != 1.0 || gcc_mi != 1.08) {
      fprintf(stderr,
              "--mortise sets GCC's increase itself; drop --gcc_ai "
              "and --gcc_mi\n");
      return 1;
    }
    // v1 with no options is the trial of 920865ec29.
    field_trials += mortise_args.empty()
                        ? "WebRTC-Tigas-Mortise/Enabled/"
                        : "WebRTC-Tigas-Mortise/Enabled," + mortise_args + "/";
  }
  const std::string detector_args = absl::GetFlag(FLAGS_gcc_detector_args);
  if (!CheckArgs("gcc_detector_args", detector_args,
                 {"window_ms", "overuse_ms", "gain", "underuse_hold"},
                 {"underuse_hold"}))
    return 1;
  if (!detector_args.empty())
    field_trials += "WebRTC-Tigas-Detector/" + detector_args + "/";
  if (absl::GetFlag(FLAGS_enable_salsify))
    field_trials += "WebRTC-Tigas-Salsify/Enabled/";
  webrtc::field_trial::InitFieldTrialsFromString(field_trials.c_str());
  if (webrtc::field_trial::IsEnabled("WebRTC-Tigas-Salsify") &&
      (absl::GetFlag(FLAGS_enable_mae) || gcc_ai != 1.0 || gcc_mi != 1.08 ||
       !detector_args.empty())) {
    fprintf(stderr,
            "Salsify uses its own fixed VBV with baseline GCC or Mortise; "
            "drop MAE and GCC experiment options\n");
    return 1;
  }

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
  options.push_fps = absl::GetFlag(FLAGS_push_fps);

  if (!WriteInfo(options.out_dir, field_trials, mortise)) {
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
