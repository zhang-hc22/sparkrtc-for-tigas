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
 */

#ifndef MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_
#define MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_

#include <stdint.h>

#include <deque>

#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"

namespace webrtc {

class TradeOffControl {
 public:
  TradeOffControl();

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
  };

  void Log(const char* branch,
           Timestamp at_time,
           double u_before,
           const Period* period) const;

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
};

}  // namespace webrtc

#endif  // MODULES_REMOTE_BITRATE_ESTIMATOR_TRADEOFF_CONTROL_H_
