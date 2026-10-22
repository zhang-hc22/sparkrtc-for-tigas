/*
 *  TIGAS: the trade-off controller with which Mortise tunes GCC, ported from
 *  AlphaRTC-mortise, branch trade-off, commit 8b09a04
 *  (modules/remote_bitrate_estimator/tradeoff_control.{h,cc}).
 *
 *  About once a second it compares Q = B - lambda * R of this period and the
 *  last, with B the mean target bitrate in Mbit/s, R the mean over feedbacks
 *  of the largest feedback_time - send_time in ms, and
 *  lambda = (B' + 0.15) / 150 from the video-call QoE, B' the last period's
 *  B. From that it moves the trade-off u in [u_min, 1], and u sets GCC's
 *  back-off factor, multiplicative increase and additive increase scale
 *  (AdjustProbingParameters). Smaller u is more aggressive.
 *
 *  The rules and constants are those of 8b09a04. Changes: the members are
 *  initialized (both u 0.8, state kProbing), the sums are 64-bit, a period
 *  with targets but no RTT or throughput sample is skipped instead of
 *  dividing by zero, the time before the first decrease is checked with
 *  IsFinite() instead of taking ms() of minus infinity, and one TigasMortise
 *  line is logged per period instead of the per-update warnings.
 *
 *  TradeOffOptions are experiment options on top of this, read from
 *  WebRTC-Tigas-Mortise/Enabled,fixed_u:..,umin_rq:..,probe:..,
 *  det_overuse_ms:..,det_window_ms:../. With none
 *  set the controller is mortise_v1, the one of 920865ec29.
 */

#ifndef MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_
#define MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_

#include <stdint.h>

#include <deque>
#include <utility>

#include "absl/strings/string_view.h"

#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"

namespace webrtc {

struct TradeOffOptions {
  // In [0, 1]: u stays at this value from the first update on and the rules
  // do not run; the periods are still logged (branch fixed).
  double fixed_u = -1.0;
  // Below 1.7 Mbit/s, u_min from R_q = R - (the least R of the periods in the
  // last 10 s) instead of R, so that it does not depend on the path RTT or
  // the receiver's feedback interval.
  bool umin_rq = false;
  // Above 0: when no rule moves u and u equals u_last, move u by this step,
  // up if u is at most u_min + 0.01, else down (branches probe_up,
  // probe_down), so that the rule has a change to compare next period.
  double probe = 0.0;
  // Above 0: GCC's overuse detector takes this overuse time and this time
  // window of the slope fit (ms) for u <= 0.8, its configured values at
  // u = 1, linear in between (DetectorShare).
  double det_overuse_ms = 0.0;
  double det_window_ms = 0.0;

  static TradeOffOptions Parse(absl::string_view trial);
  bool Detector() const { return det_overuse_ms > 0.0 || det_window_ms > 0.0; }
  bool Any() const {
    return fixed_u >= 0.0 || umin_rq || probe > 0.0 || Detector();
  }
};

class TradeOffControl {
 public:
  explicit TradeOffControl(const TradeOffOptions& options = TradeOffOptions());

  // One feedback: its largest feedback_time - send_time and the acknowledged
  // bitrate.
  void UpdateTradeOffEstimate(TimeDelta rtt, DataRate throughput);
  // The target bitrate after each AIMD update.
  void UpdateTargetBitrate(Timestamp at_time, DataRate target_bitrate);
  // GCC's back-off factor, per-second multiplicative increase and additive
  // increase scale for the current trade-off.
  void AdjustProbingParameters(double* beta,
                               double* alpha,
                               double* additive_coef) const;
  void GetQoeLambda();
  // Called before each AIMD update. Returns true if the trade-off changed.
  bool UpdateTradeoff(Timestamp at_time);
  const TradeOffOptions& options() const { return options_; }
  // How far the detector moves from its configured values to the det_*
  // options at the current u: min(1, (1 - u) / 0.2).
  double DetectorShare() const;

 private:
  enum class TradeoffProbingState {
    kProbing,
    kChangePointDetected,
  };

  // The means of one period, for the log.
  struct Period {
    int64_t bitrate_bps;
    int64_t tput_bps;
    int64_t rtt_ms;
    double qoe;
    double last_qoe;
    double min_delta;
    // umin_rq only: the least R of the periods in the last 10 s.
    int64_t rtt_min_ms;
  };

  // Closes the period at at_time into period, or returns the skip branch.
  const char* ClosePeriod(Timestamp at_time, Period* period);
  bool UpdateFixed(Timestamp at_time);

  void Log(const char* branch,
           Timestamp at_time,
           double u_before,
           const Period* period) const;

  const TradeOffOptions options_;
  TradeoffProbingState tradeoff_state_;
  Timestamp time_last_decrease_bitrate_;
  Timestamp time_last_change_tradeoff_;

  double cur_trade_off_;
  double last_trade_off_;
  double qoe_lambda_;

  DataRate last_period_bitrate_;
  DataRate last_period_tput_;
  TimeDelta last_period_rtt_;

  std::deque<int64_t> network_rtts_;
  std::deque<int64_t> target_bitrates_;
  std::deque<int64_t> network_tputs_;
  // umin_rq: (end of period, R) of the periods in the last 10 s.
  std::deque<std::pair<Timestamp, int64_t>> period_rtts_;
};

}  // namespace webrtc

#endif  // MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_
