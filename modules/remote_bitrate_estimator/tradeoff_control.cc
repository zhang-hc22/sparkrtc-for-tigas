/*
 *  TIGAS: see tradeoff_control.h. Line references are to 8b09a04.
 */

#include "modules/remote_bitrate_estimator/tradeoff_control.h"

#include <algorithm>
#include <numeric>

#include "rtc_base/logging.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
namespace {

constexpr double kMaxTradeOff = 1.0;
constexpr int64_t kChangePointIntervalMs = 3000;
constexpr int64_t kPeriodMs = 1000;

// 8b09a04 sums into an int (std::accumulate(..., 0)), which overflows at
// about 2.1e9 bit/s summed over a period.
int64_t Mean(const std::deque<int64_t>& values) {
  return std::accumulate(values.begin(), values.end(), int64_t{0}) /
         static_cast<int64_t>(values.size());
}

}  // namespace

TradeOffControl::TradeOffControl()
    : tradeoff_state_(TradeoffProbingState::kProbing),
      time_last_decrease_bitrate_(Timestamp::MinusInfinity()),
      time_last_change_tradeoff_(Timestamp::MinusInfinity()),
      cur_trade_off_(0.8),
      last_trade_off_(0.8),
      qoe_lambda_(0.0),
      last_period_bitrate_(DataRate::KilobitsPerSec(1000)),
      last_period_tput_(DataRate::KilobitsPerSec(1000)),
      last_period_rtt_(TimeDelta::Millis(200)) {}

void TradeOffControl::UpdateTradeOffEstimate(TimeDelta rtt,
                                             DataRate throughput) {
  if (rtt.IsZero() || !rtt.IsFinite())
    return;
  network_rtts_.push_back(rtt.ms());
  network_tputs_.push_back(throughput.bps());
}

void TradeOffControl::UpdateTargetBitrate(Timestamp at_time,
                                          DataRate target_bitrate) {
  if (target_bitrate.bps() > 0) {
    if (!target_bitrates_.empty() &&
        (target_bitrate.bps() < target_bitrates_.back() ||
         time_last_decrease_bitrate_.IsInfinite()))
      time_last_decrease_bitrate_ = at_time;
    target_bitrates_.push_back(target_bitrate.bps());
  }
}

void TradeOffControl::AdjustProbingParameters(double* beta,
                                              double* alpha,
                                              double* additive_coef) const {
  // u = 0.8 gives GCC's back-off factor 0.85.
  *beta = 0.93 - 0.1 * cur_trade_off_;
  *alpha = 1.01 + 0.3 * (1.0 - cur_trade_off_);
  *additive_coef = 0.7 + 20 * (1.0 - cur_trade_off_);
}

void TradeOffControl::GetQoeLambda() {
  qoe_lambda_ = (last_period_bitrate_.kbps() / 1000.0 + 0.15) / 150.0;
}

bool TradeOffControl::UpdateTradeoff(Timestamp at_time) {
  const double u_before = cur_trade_off_;
  if (time_last_change_tradeoff_.IsMinusInfinity())
    time_last_change_tradeoff_ = at_time;
  // 8b09a04 takes ms() of the minus-infinite time before the first decrease,
  // which reads as long ago.
  const bool decreased_recently =
      time_last_decrease_bitrate_.IsFinite() &&
      at_time.ms() - time_last_decrease_bitrate_.ms() < kChangePointIntervalMs;

  if (at_time.ms() - time_last_change_tradeoff_.ms() < kPeriodMs) {
    if (decreased_recently ||
        tradeoff_state_ != TradeoffProbingState::kProbing)
      return false;
    // No decrease for 3 s: the bandwidth went up a lot.
    last_trade_off_ = cur_trade_off_;
    if (last_period_bitrate_.kbps() < 1000)
      cur_trade_off_ = 0.2 + last_period_bitrate_.kbps() / 5000.0;
    else
      cur_trade_off_ = 0.3 + last_period_bitrate_.kbps() / 5000.0;
    cur_trade_off_ = std::min(0.9, cur_trade_off_);
    time_last_change_tradeoff_ = at_time;
    tradeoff_state_ = TradeoffProbingState::kChangePointDetected;
    target_bitrates_.clear();
    network_rtts_.clear();
    network_tputs_.clear();
    Log("cp_enter", at_time, u_before, nullptr);
    return true;
  }

  time_last_change_tradeoff_ = at_time;
  if (target_bitrates_.empty()) {
    Log("skip_no_target", at_time, u_before, nullptr);
    return false;
  }
  // 8b09a04 divides by zero here.
  if (network_tputs_.empty() || network_rtts_.empty()) {
    target_bitrates_.clear();
    network_rtts_.clear();
    network_tputs_.clear();
    Log("skip_no_sample", at_time, u_before, nullptr);
    return false;
  }

  Period period;
  period.bitrate_bps = Mean(target_bitrates_);
  period.tput_bps = Mean(network_tputs_);
  period.rtt_ms = Mean(network_rtts_);
  const DataRate cur_bitrate = DataRate::BitsPerSec(period.bitrate_bps);
  const DataRate cur_tput = DataRate::BitsPerSec(period.tput_bps);
  const TimeDelta cur_rtt = TimeDelta::Millis(period.rtt_ms);

  target_bitrates_.clear();
  network_rtts_.clear();
  network_tputs_.clear();

  const double cur_qoe = (static_cast<double>(cur_bitrate.kbps()) / 1000.0) -
                         qoe_lambda_ * static_cast<double>(cur_rtt.ms());
  const double last_qoe =
      (static_cast<double>(last_period_bitrate_.kbps()) / 1000.0) -
      qoe_lambda_ * static_cast<double>(last_period_rtt_.ms());

  // Keep it from being over-aggressive.
  double min_delta =
      0.15 * std::min(static_cast<double>(cur_rtt.ms()), 250.0) / 50.0 + 0.15;
  if (cur_bitrate.kbps() > 1700)
    min_delta = 0.7;
  if (cur_bitrate.kbps() > 2200)
    min_delta = 0.8;
  if (cur_bitrate.kbps() > 2500)
    min_delta = 0.9;
  period.qoe = cur_qoe;
  period.last_qoe = last_qoe;
  period.min_delta = min_delta;

  last_period_rtt_ = cur_rtt;
  last_period_tput_ = cur_tput;
  last_period_bitrate_ = cur_bitrate;

  // After a change point, wait for the next decrease, lowering u meanwhile.
  if (tradeoff_state_ == TradeoffProbingState::kChangePointDetected) {
    last_trade_off_ = cur_trade_off_;
    if (decreased_recently) {
      cur_trade_off_ = std::max(cur_trade_off_, min_delta);
      cur_trade_off_ = std::min(kMaxTradeOff, cur_trade_off_);
      tradeoff_state_ = TradeoffProbingState::kProbing;
      Log("cp_exit", at_time, u_before, &period);
    } else {
      cur_trade_off_ = cur_trade_off_ / 1.81;
      cur_trade_off_ = std::max(min_delta, cur_trade_off_);
      cur_trade_off_ = std::min(kMaxTradeOff, cur_trade_off_);
      Log("cp_decay", at_time, u_before, &period);
    }
    return true;
  }

  if ((cur_trade_off_ > last_trade_off_ && cur_qoe > last_qoe) ||
      (cur_trade_off_ < last_trade_off_ && cur_qoe < last_qoe)) {
    last_trade_off_ = cur_trade_off_;
    cur_trade_off_ = cur_trade_off_ + 0.2;
    cur_trade_off_ = std::min(kMaxTradeOff, cur_trade_off_);
    Log("rule_up", at_time, u_before, &period);
    return true;
  } else if ((cur_trade_off_ < last_trade_off_ && cur_qoe > last_qoe) ||
             (cur_trade_off_ > last_trade_off_ && cur_qoe < last_qoe)) {
    last_trade_off_ = cur_trade_off_;
    cur_trade_off_ = cur_trade_off_ / 1.78;
    cur_trade_off_ = std::max(min_delta, cur_trade_off_);
    cur_trade_off_ = std::min(kMaxTradeOff, cur_trade_off_);
    Log("rule_down", at_time, u_before, &period);
    return true;
  }
  Log("rule_hold", at_time, u_before, &period);
  return false;
}

void TradeOffControl::Log(const char* branch,
                          Timestamp at_time,
                          double u_before,
                          const Period* period) const {
  double beta, alpha, additive_coef;
  AdjustProbingParameters(&beta, &alpha, &additive_coef);
  const int64_t since_decrease_ms =
      time_last_decrease_bitrate_.IsFinite()
          ? at_time.ms() - time_last_decrease_bitrate_.ms()
          : -1;
  // last_bitrate is the B' of the next lambda; after a period it is bitrate.
  rtc::StringBuilder sb;
  sb << "TigasMortise utc_ms=" << rtc::TimeUTCMillis()
     << " at_ms=" << at_time.ms() << " branch=" << branch
     << " u_before=" << u_before << " u=" << cur_trade_off_
     << " u_last=" << last_trade_off_ << " beta=" << beta
     << " alpha=" << alpha << " ai=" << additive_coef
     << " lambda=" << qoe_lambda_
     << " since_decrease_ms=" << since_decrease_ms
     << " last_bitrate=" << last_period_bitrate_.bps();
  if (period) {
    sb << " bitrate=" << period->bitrate_bps << " tput=" << period->tput_bps
       << " rtt=" << period->rtt_ms << " q=" << period->qoe
       << " q_last=" << period->last_qoe << " u_min=" << period->min_delta;
  }
  RTC_LOG(LS_INFO) << sb.str();
}

}  // namespace webrtc
