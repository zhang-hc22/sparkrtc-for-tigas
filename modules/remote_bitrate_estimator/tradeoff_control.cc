/*
 *  TIGAS: see tradeoff_control.h. Line references are to 8b09a04.
 */

#include "modules/remote_bitrate_estimator/tradeoff_control.h"

#include <stdio.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/logging.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
namespace {

constexpr double kMaxTradeOff = 1.0;
constexpr int64_t kChangePointIntervalMs = 3000;
constexpr int64_t kPeriodMs = 1000;
// umin_rq: the window of the least R.
constexpr TimeDelta kRttMinWindow = TimeDelta::Seconds(10);
// v3: the coarse steps of v1 and v2, which also bound a fine step.
constexpr double kStepDown = 1.78;
constexpr double kStepUp = 0.2;
// v3: build_detector's hazard and Student-t prior.
constexpr double kHazard = 1.0 / 250.0;
constexpr double kAlpha0 = 0.1;
constexpr double kBeta0 = 0.01;
constexpr double kKappa0 = 1.0;
constexpr double kMu0 = 0.0;
// v4: the model (tradeoff_control.h DecideTradeOffV4, tigas/exp/mortise_v4.py):
// ln eta(u), ln phi(u) and psi(u) (ms) as a u + b u^2 + c u^3, VMAF of the
// rate, the queue scale of w, the least share of eta and the headroom over
// the acknowledged rate, the periods of the standing queue, the half width
// of the slope, the grid of the fine target and the window of the least R.
constexpr double kEta[3] = {-0.9734, 1.2983, -1.4818};
constexpr double kPhi[3] = {-2.0068, 1.6873, -1.97};
constexpr double kPsi[3] = {-2.47, -137.81, 92.02};
constexpr double kVmafMax = 99.912;
constexpr double kVmafScale = 0.606;
constexpr double kVmafShape = 1.558;
constexpr double kQueueScaleMs = 15.0;
constexpr double kEtaShareMin = 0.3;
constexpr double kHeadroom = 0.5;
constexpr size_t kStandPeriods = 3;
constexpr double kSlopeHalf = 0.05;
constexpr double kGrid = 0.01;
constexpr TimeDelta kRttBaseWindow = TimeDelta::Seconds(60);

// The density of the Student-t distribution, as scipy.stats.t.pdf.
double StudentTPdf(double x, double df, double loc, double scale) {
  const double z = (x - loc) / scale;
  return std::exp(std::lgamma((df + 1.0) / 2.0) - std::lgamma(df / 2.0)) /
         (std::sqrt(df * M_PI) * scale) *
         std::pow(1.0 + z * z / df, -(df + 1.0) / 2.0);
}

double Cubic(const double (&c)[3], double u) {
  return u * (c[0] + u * (c[1] + u * c[2]));
}

double Vmaf(double b_mbps) {
  return kVmafMax *
         (1.0 - std::exp(-std::pow(std::max(b_mbps, 0.0) / kVmafScale,
                                   kVmafShape)));
}

// Prints a double so that the offline replay reads it back exactly enough.
std::string Num(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.9g", v);
  return buf;
}

// 8b09a04 sums into an int (std::accumulate(..., 0)), which overflows at
// about 2.1e9 bit/s summed over a period.
int64_t Mean(const std::deque<int64_t>& values) {
  return std::accumulate(values.begin(), values.end(), int64_t{0}) /
         static_cast<int64_t>(values.size());
}

}  // namespace

TradeOffOptions TradeOffOptions::Parse(absl::string_view trial) {
  FieldTrialParameter<double> fixed_u("fixed_u", -1.0);
  FieldTrialParameter<bool> umin_rq("umin_rq", false);
  FieldTrialParameter<double> probe("probe", 0.0);
  FieldTrialParameter<double> det_overuse_ms("det_overuse_ms", 0.0);
  FieldTrialParameter<double> det_window_ms("det_window_ms", 0.0);
  FieldTrialParameter<bool> v3("v3", false);
  FieldTrialParameter<int> v3_hist("v3_hist", 6);
  FieldTrialParameter<double> v3_sigma("v3_sigma", 0.5);
  FieldTrialParameter<double> v3_dither("v3_dither", 0.05);
  FieldTrialParameter<bool> v3_fine("v3_fine", true);
  FieldTrialParameter<int> v3_est("v3_est", 0);
  FieldTrialParameter<bool> umin_hr("umin_hr", true);
  FieldTrialParameter<bool> v4("v4", false);
  FieldTrialParameter<int> v4_hist("v4_hist", 5);
  FieldTrialParameter<double> v4_umin("v4_umin", 0.1);
  FieldTrialParameter<double> v4_umax("v4_umax", 0.8);
  FieldTrialParameter<double> v4_sigma("v4_sigma", 0.5);
  FieldTrialParameter<bool> v4_hold("v4_hold", true);
  FieldTrialParameter<int> v4_wait("v4_wait", 1);
  FieldTrialParameter<bool> v4_median("v4_median", false);
  ParseFieldTrial({&fixed_u, &umin_rq, &probe, &det_overuse_ms,
                   &det_window_ms, &v3, &v3_hist, &v3_sigma, &v3_dither,
                   &v3_fine, &v3_est, &umin_hr, &v4, &v4_hist, &v4_umin,
                   &v4_umax, &v4_sigma, &v4_hold, &v4_wait, &v4_median},
                  trial);
  TradeOffOptions options;
  options.fixed_u = std::min(fixed_u.Get(), kMaxTradeOff);
  options.umin_rq = umin_rq.Get();
  options.probe = probe.Get();
  options.det_overuse_ms = det_overuse_ms.Get();
  options.det_window_ms = det_window_ms.Get();
  options.v3 = v3.Get();
  options.v3_hist = std::max(2, v3_hist.Get());
  options.v3_sigma = std::min(1.0, std::max(0.0, v3_sigma.Get()));
  options.v3_dither = v3_dither.Get();
  options.v3_fine = v3_fine.Get();
  options.v3_est = v3_est.Get();
  options.umin_hr = umin_hr.Get();
  options.v4 = v4.Get();
  options.v4_hist = std::max(1, v4_hist.Get());
  options.v4_umax = std::min(kMaxTradeOff, std::max(0.0, v4_umax.Get()));
  options.v4_umin = std::min(options.v4_umax, std::max(0.0, v4_umin.Get()));
  options.v4_sigma = std::min(1.0, std::max(0.0, v4_sigma.Get()));
  options.v4_hold = v4_hold.Get();
  options.v4_wait = std::max(0, v4_wait.Get());
  options.v4_median = v4_median.Get();
  return options;
}

ChangePointDetector::ChangePointDetector()
    : r_(kHistoryLen + 2, 0.0),
      alpha_{kAlpha0},
      beta_{kBeta0},
      kappa_{kKappa0},
      mu_{kMu0} {
  r_[0] = 1.0;
}

int ChangePointDetector::AddData(double x) {
  ++t_;
  if (t_ - t0_ > kHistoryLen) {
    // prune: keep the parameters of run lengths 0 to kHistoryLen.
    t0_ = t_ - kHistoryLen;
    const size_t keep = t_ - t0_ + 1;
    alpha_.resize(keep);
    beta_.resize(keep);
    kappa_.resize(keep);
    mu_.resize(keep);
  }
  const int t = t_ - t0_;
  std::vector<double> pred(t + 1);
  for (int i = 0; i <= t; ++i) {
    pred[i] = StudentTPdf(
        x, 2.0 * alpha_[i], mu_[i],
        std::sqrt(beta_[i] * (kappa_[i] + 1.0) / (alpha_[i] * kappa_[i])));
  }
  double cp_prob = 0.0;
  for (int i = 0; i <= t; ++i)
    cp_prob += r_[i] * pred[i] * kHazard;
  for (int i = t; i >= 0; --i)
    r_[i + 1] = r_[i] * pred[i] * (1.0 - kHazard);
  r_[0] = cp_prob;
  double sum = 0.0;
  for (int i = 0; i <= t + 1; ++i)
    sum += r_[i];
  if (sum > 0.0 && std::isfinite(sum)) {
    for (int i = 0; i <= t + 1; ++i)
      r_[i] /= sum;
  } else {
    // change_point.py would carry NaN from here on; start a run instead.
    std::fill(r_.begin(), r_.end(), 0.0);
    r_[0] = 1.0;
  }
  // update_theta: the prior first, then each run length one longer.
  std::vector<double> alpha{kAlpha0}, beta{kBeta0}, kappa{kKappa0},
      mu{kMu0};
  for (size_t i = 0; i < mu_.size(); ++i) {
    mu.push_back((kappa_[i] * mu_[i] + x) / (kappa_[i] + 1.0));
    kappa.push_back(kappa_[i] + 1.0);
    alpha.push_back(alpha_[i] + 0.5);
    beta.push_back(beta_[i] + (kappa_[i] * (x - mu_[i]) * (x - mu_[i])) /
                                  (2.0 * (kappa_[i] + 1.0)));
  }
  alpha_.swap(alpha);
  beta_.swap(beta);
  kappa_.swap(kappa);
  mu_.swap(mu);
  return static_cast<int>(std::max_element(r_.begin(), r_.end()) - r_.begin());
}

TradeOffDecision DecideTradeOff(const std::deque<TradeOffPoint>& history,
                                double lambda,
                                double u,
                                double u_min,
                                bool fine,
                                int est) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  TradeOffDecision d{"hold", u, nan, nan, nan};
  const double n = static_cast<double>(history.size());
  if (est == 1 && history.size() >= 2) {
    double mean_u = 0.0, mean_r = 0.0, mean_b = 0.0;
    for (const auto& p : history) {
      mean_u += p.u;
      mean_r += static_cast<double>(p.rtt_ms);
      mean_b += static_cast<double>(p.bitrate_bps) / 1e6;
    }
    mean_u /= n;
    mean_r /= n;
    mean_b /= n;
    double suu = 0.0, sur = 0.0, sub = 0.0;
    for (const auto& p : history) {
      const double du = p.u - mean_u;
      suu += du * du;
      sur += du * (static_cast<double>(p.rtt_ms) - mean_r);
      sub += du * (static_cast<double>(p.bitrate_bps) / 1e6 - mean_b);
    }
    // Valid if u has a standard deviation of at least 0.01.
    if (suu / n >= 1e-4) {
      const double db = sub / suu, dr = sur / suu;
      d.dq = db - lambda * dr;
      // A smaller u should add delay; otherwise there is no slope to compare.
      if (dr < 0.0) {
        d.tau = db / dr;
        d.ratio = lambda > 0.0 ? d.tau / lambda : nan;
      }
    }
  } else if (est == 0 && history.size() >= 2) {
    double mean_r = 0.0, mean_b = 0.0;
    for (const auto& p : history) {
      mean_r += static_cast<double>(p.rtt_ms);
      mean_b += static_cast<double>(p.bitrate_bps) / 1e6;
    }
    mean_r /= n;
    mean_b /= n;
    double srr = 0.0, srb = 0.0;
    for (const auto& p : history) {
      const double dr = static_cast<double>(p.rtt_ms) - mean_r;
      srr += dr * dr;
      srb += dr * (static_cast<double>(p.bitrate_bps) / 1e6 - mean_b);
    }
    // The slope is valid if R has a standard deviation of at least 1 ms.
    if (srr / n >= 1.0) {
      d.tau = srb / srr;
      d.ratio = lambda > 0.0 ? d.tau / lambda : nan;
    }
  }
  if (!std::isnan(d.tau) || !std::isnan(d.dq)) {
    bool fitted = false;
    if (fine && d.ratio > 0.5 && d.ratio < 2.0) {
      // Q = B - lambda R on z = (u_i - u) / 0.1, least squares a z^2 + b z
      // + c; needs 3 distinct u_i and a < 0.
      std::vector<double> us;
      for (const auto& p : history)
        us.push_back(p.u);
      std::sort(us.begin(), us.end());
      int distinct = 1;
      for (size_t i = 1; i < us.size(); ++i)
        distinct += us[i] - us[i - 1] > 1e-3 ? 1 : 0;
      if (distinct >= 3) {
        // Normal equations m * (a, b, c) = v.
        double m[3][4] = {};
        for (const auto& p : history) {
          const double z = (p.u - u) / 0.1;
          const double q = static_cast<double>(p.bitrate_bps) / 1e6 -
                           lambda * static_cast<double>(p.rtt_ms);
          const double f[3] = {z * z, z, 1.0};
          for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j)
              m[i][j] += f[i] * f[j];
            m[i][3] += f[i] * q;
          }
        }
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) {
          int pivot = c;
          for (int i = c + 1; i < 3; ++i)
            if (std::fabs(m[i][c]) > std::fabs(m[pivot][c]))
              pivot = i;
          if (std::fabs(m[pivot][c]) < 1e-12) {
            ok = false;
            break;
          }
          for (int j = 0; j < 4; ++j)
            std::swap(m[c][j], m[pivot][j]);
          for (int i = 0; i < 3; ++i) {
            if (i == c)
              continue;
            const double k = m[i][c] / m[c][c];
            for (int j = c; j < 4; ++j)
              m[i][j] -= k * m[c][j];
          }
        }
        if (ok) {
          const double a = m[0][3] / m[0][0];
          const double b = m[1][3] / m[1][1];
          if (a < 0.0) {
            const double vertex = u + 0.1 * (-b / (2.0 * a));
            d.u_pref = std::min(u + kStepUp, std::max(u / kStepDown, vertex));
            d.mode = "fine";
            fitted = true;
          }
        }
      }
    }
    if (!fitted) {
      // A steeper slope than lambda: more bitrate per ms is worth it. Along
      // u: Q rises as u falls.
      const bool down = est == 1 ? d.dq < 0.0 : d.tau > lambda;
      d.mode = down ? "coarse_down" : "coarse_up";
      d.u_pref = down ? u / kStepDown : u + kStepUp;
    }
  }
  d.u_pref = std::min(kMaxTradeOff, std::max(u_min, d.u_pref));
  return d;
}

TradeOffDecisionV4 DecideTradeOffV4(
    const std::deque<TradeOffSampleV4>& history,
    double u,
    double u_lo,
    double u_hi,
    bool median) {
  TradeOffDecisionV4 d{"coarse_up", u, 0.0, 0.0, 1.0};
  // The queue that stayed over the last periods, moved to u = 0; new lows
  // left out unless all are.
  const size_t first =
      history.size() > kStandPeriods ? history.size() - kStandPeriods : 0;
  std::vector<double> qs, all;
  for (size_t i = first; i < history.size(); ++i) {
    const double x = history[i].q_ms * std::exp(-Cubic(kPhi, history[i].u));
    all.push_back(x);
    if (!history[i].low)
      qs.push_back(x);
  }
  if (qs.empty())
    qs = all;
  std::sort(qs.begin(), qs.end());
  const size_t n_qs = qs.size();
  const double q0 = !median        ? qs.front()
                    : n_qs % 2 == 1 ? qs[n_qs / 2]
                                    : 0.5 * (qs[n_qs / 2 - 1] + qs[n_qs / 2]);
  d.q_stand = q0 * std::exp(Cubic(kPhi, history.back().u));
  d.w = std::exp(-d.q_stand / kQueueScaleMs);
  const double share = kEtaShareMin + (1.0 - kEtaShareMin) * d.w;
  const double cap = 1.0 + kHeadroom * d.w;
  auto rate_part = [&](double x) {
    const double e = Cubic(kEta, x);
    double sum = 0.0;
    for (const auto& p : history) {
      const double b = std::min(
          static_cast<double>(p.bitrate_bps) *
              std::exp((e - Cubic(kEta, p.u)) * share),
          static_cast<double>(p.tput_bps) * cap);
      sum += Vmaf(b / 1e6);
    }
    return sum / static_cast<double>(history.size());
  };
  auto latency = [&](double x) {
    return Cubic(kPsi, x) + q0 * std::exp(Cubic(kPhi, x));
  };
  const double a = std::max(u_lo, u - kSlopeHalf);
  const double b = std::min(u_hi, u + kSlopeHalf);
  const double gain = 0.5 * (rate_part(a) - rate_part(b));
  const double cost = 0.1 * (latency(a) - latency(b));
  d.ratio = cost > 1e-9 ? gain / cost : std::numeric_limits<double>::infinity();
  if (d.ratio > 0.5 && d.ratio < 2.0) {
    double best = -std::numeric_limits<double>::infinity();
    const int n = static_cast<int>(std::lround((u_hi - u_lo) / kGrid));
    for (int j = 0; j <= n; ++j) {
      const double x = u_lo + kGrid * j;
      const double v = 0.5 * rate_part(x) - 0.1 * latency(x);
      if (v > best) {
        best = v;
        d.u_pref = x;
      }
    }
    d.mode = "fine";
  } else if (d.ratio >= 2.0) {
    d.mode = "coarse_down";
    d.u_pref = u / kStepDown;
  } else {
    d.mode = "coarse_up";
    d.u_pref = u + kStepUp;
  }
  d.u_pref = std::min(u_hi, std::max(u_lo, d.u_pref));
  return d;
}

TradeOffControl::TradeOffControl(const TradeOffOptions& options)
    : options_(options),
      tradeoff_state_(TradeoffProbingState::kProbing),
      time_last_decrease_bitrate_(Timestamp::MinusInfinity()),
      time_last_change_tradeoff_(Timestamp::MinusInfinity()),
      cur_trade_off_(options.fixed_u >= 0.0 ? options.fixed_u : 0.8),
      last_trade_off_(options.fixed_u >= 0.0 ? options.fixed_u : 0.8),
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
  if (options_.v3 || options_.v4) {
    // calc_opt_delta.py check_change_point, on the EWMA (weight 0.2) of the
    // acknowledged rate of each feedback.
    ewma_rate_ = 0.8 * ewma_rate_ +
                 0.2 * static_cast<double>(throughput.bps()) / 1048576.0;
    const int run_len = cp_detector_.AddData(ewma_rate_);
    const bool detected =
        run_len_ < run_len && run_len < last_run_len_ && run_len <= 10;
    cp_detected_ |= detected;
    last_run_len_ = run_len_;
    run_len_ = run_len;
    RTC_LOG(LS_INFO) << "TigasCp x=" << Num(ewma_rate_)
                     << " run_len=" << run_len << " cp=" << (detected ? 1 : 0);
  }
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

const char* TradeOffControl::ClosePeriod(Timestamp at_time, Period* period) {
  if (target_bitrates_.empty())
    return "skip_no_target";
  // 8b09a04 divides by zero here.
  if (network_tputs_.empty() || network_rtts_.empty()) {
    target_bitrates_.clear();
    network_rtts_.clear();
    network_tputs_.clear();
    return "skip_no_sample";
  }

  period->bitrate_bps = Mean(target_bitrates_);
  period->tput_bps = Mean(network_tputs_);
  period->rtt_ms = Mean(network_rtts_);
  const DataRate cur_bitrate = DataRate::BitsPerSec(period->bitrate_bps);
  const DataRate cur_tput = DataRate::BitsPerSec(period->tput_bps);
  const TimeDelta cur_rtt = TimeDelta::Millis(period->rtt_ms);

  target_bitrates_.clear();
  network_rtts_.clear();
  network_tputs_.clear();

  const double cur_qoe = (static_cast<double>(cur_bitrate.kbps()) / 1000.0) -
                         qoe_lambda_ * static_cast<double>(cur_rtt.ms());
  const double last_qoe =
      (static_cast<double>(last_period_bitrate_.kbps()) / 1000.0) -
      qoe_lambda_ * static_cast<double>(last_period_rtt_.ms());

  // Keep it from being over-aggressive.
  double rtt_for_min = static_cast<double>(cur_rtt.ms());
  period->rtt_min_ms = -1;
  if (options_.umin_rq) {
    period_rtts_.emplace_back(at_time, cur_rtt.ms());
    while (at_time - period_rtts_.front().first >= kRttMinWindow)
      period_rtts_.pop_front();
    period->rtt_min_ms = cur_rtt.ms();
    for (const auto& p : period_rtts_)
      period->rtt_min_ms = std::min(period->rtt_min_ms, p.second);
    rtt_for_min = static_cast<double>(cur_rtt.ms() - period->rtt_min_ms);
  }
  double min_delta = 0.15 * std::min(rtt_for_min, 250.0) / 50.0 + 0.15;
  if (options_.umin_hr) {
    if (cur_bitrate.kbps() > 1700)
      min_delta = 0.7;
    if (cur_bitrate.kbps() > 2200)
      min_delta = 0.8;
    if (cur_bitrate.kbps() > 2500)
      min_delta = 0.9;
  }
  period->qoe = cur_qoe;
  period->last_qoe = last_qoe;
  period->min_delta = min_delta;

  last_period_rtt_ = cur_rtt;
  last_period_tput_ = cur_tput;
  last_period_bitrate_ = cur_bitrate;
  return nullptr;
}

double TradeOffControl::DetectorShare() const {
  return std::min(1.0, std::max(0.0, (1.0 - cur_trade_off_) / 0.2));
}

bool TradeOffControl::UpdateFixed(Timestamp at_time) {
  const double u = cur_trade_off_;
  if (time_last_change_tradeoff_.IsMinusInfinity()) {
    // The first update: GCC takes the parameters of the fixed u.
    time_last_change_tradeoff_ = at_time;
    Log("fixed_init", at_time, u, nullptr);
    return true;
  }
  if (at_time.ms() - time_last_change_tradeoff_.ms() < kPeriodMs)
    return false;
  time_last_change_tradeoff_ = at_time;
  Period period;
  const char* skip = ClosePeriod(at_time, &period);
  Log(skip ? skip : "fixed", at_time, u, skip ? nullptr : &period);
  return false;
}

bool TradeOffControl::UpdateTradeoff(Timestamp at_time) {
  if (options_.fixed_u >= 0.0)
    return UpdateFixed(at_time);
  if (options_.v4)
    return UpdateV4(at_time);
  if (options_.v3)
    return UpdateV3(at_time);
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
  Period period;
  if (const char* skip = ClosePeriod(at_time, &period)) {
    Log(skip, at_time, u_before, nullptr);
    return false;
  }
  const double cur_qoe = period.qoe;
  const double last_qoe = period.last_qoe;
  const double min_delta = period.min_delta;

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
  if (options_.probe > 0.0 && cur_trade_off_ == last_trade_off_) {
    const bool up = cur_trade_off_ <= min_delta + 0.01;
    last_trade_off_ = cur_trade_off_;
    cur_trade_off_ += up ? options_.probe : -options_.probe;
    cur_trade_off_ = std::max(min_delta, cur_trade_off_);
    cur_trade_off_ = std::min(kMaxTradeOff, cur_trade_off_);
    Log(up ? "probe_up" : "probe_down", at_time, u_before, &period);
    return true;
  }
  Log("rule_hold", at_time, u_before, &period);
  return false;
}

bool TradeOffControl::UpdateV3(Timestamp at_time) {
  const double u_before = cur_trade_off_;
  if (time_last_change_tradeoff_.IsMinusInfinity())
    time_last_change_tradeoff_ = at_time;
  if (at_time.ms() - time_last_change_tradeoff_.ms() < kPeriodMs)
    return false;
  time_last_change_tradeoff_ = at_time;
  Period period;
  if (const char* skip = ClosePeriod(at_time, &period)) {
    Log(skip, at_time, u_before, nullptr);
    return false;
  }
  const double u_min = period.min_delta;
  v3_history_.push_back({u_before, period.bitrate_bps, period.rtt_ms});
  while (static_cast<int>(v3_history_.size()) > options_.v3_hist)
    v3_history_.pop_front();
  const TradeOffDecision d = DecideTradeOff(v3_history_, qoe_lambda_,
                                            u_before, u_min, options_.v3_fine,
                                            options_.v3_est);

  // The snapshot of the decision, for the offline replay.
  rtc::StringBuilder sb;
  sb << "TigasMortiseV3 at_ms=" << at_time.ms()
     << " lambda=" << Num(qoe_lambda_) << " u=" << Num(u_before)
     << " u_min=" << Num(u_min) << " cp=" << (cp_detected_ ? 1 : 0)
     << " mode=" << d.mode << " u_pref=" << Num(d.u_pref)
     << " tau=" << Num(d.tau) << " dq=" << Num(d.dq) << " hist_u=";
  for (size_t i = 0; i < v3_history_.size(); ++i)
    sb << (i ? ";" : "") << Num(v3_history_[i].u);
  sb << " hist_b=";
  for (size_t i = 0; i < v3_history_.size(); ++i)
    sb << (i ? ";" : "") << v3_history_[i].bitrate_bps;
  sb << " hist_r=";
  for (size_t i = 0; i < v3_history_.size(); ++i)
    sb << (i ? ";" : "") << v3_history_[i].rtt_ms;
  RTC_LOG(LS_INFO) << sb.str();

  last_trade_off_ = cur_trade_off_;
  std::string branch;
  double u;
  if (cp_detected_) {
    // A change point: go to the target and start a new history.
    u = d.u_pref;
    v3_history_.clear();
    v3_still_ = 0;
    cp_detected_ = false;
    branch = "v3_cp";
  } else {
    u = options_.v3_sigma * d.u_pref + (1.0 - options_.v3_sigma) * u_before;
    u = std::min(kMaxTradeOff, std::max(u_min, u));
    branch = std::string("v3_") + d.mode;
    v3_still_ = std::fabs(u - u_before) < 0.02 ? v3_still_ + 1 : 0;
    if (options_.v3_dither > 0.0 && v3_still_ >= 3) {
      const bool up = u <= u_min + 0.01;
      u += up ? options_.v3_dither : -options_.v3_dither;
      u = std::min(kMaxTradeOff, std::max(u_min, u));
      v3_still_ = 0;
      branch = up ? "v3_dither_up" : "v3_dither_down";
    }
  }
  cur_trade_off_ = u;
  Log(branch.c_str(), at_time, u_before, &period);
  return u != u_before;
}

bool TradeOffControl::UpdateV4(Timestamp at_time) {
  const double u_before = cur_trade_off_;
  if (time_last_change_tradeoff_.IsMinusInfinity())
    time_last_change_tradeoff_ = at_time;
  if (at_time.ms() - time_last_change_tradeoff_.ms() < kPeriodMs)
    return false;
  time_last_change_tradeoff_ = at_time;
  Period period;
  if (const char* skip = ClosePeriod(at_time, &period)) {
    Log(skip, at_time, u_before, nullptr);
    return false;
  }
  // The u_min floors do not apply; the log shows v4's lower bound.
  period.min_delta = options_.v4_umin;
  while (!v4_rtts_.empty() &&
         at_time - v4_rtts_.front().first >= kRttBaseWindow)
    v4_rtts_.pop_front();
  bool low = true;
  for (const auto& p : v4_rtts_)
    low = low && period.rtt_ms < p.second;
  v4_rtts_.emplace_back(at_time, period.rtt_ms);
  int64_t rtt_base = period.rtt_ms;
  for (const auto& p : v4_rtts_)
    rtt_base = std::min(rtt_base, p.second);
  // GCC follows u with a lag of about a period.
  const double u_eff = 0.5 * (last_trade_off_ + u_before);
  v4_history_.push_back({u_eff, period.bitrate_bps, period.tput_bps,
                         static_cast<double>(period.rtt_ms - rtt_base), low});
  while (static_cast<int>(v4_history_.size()) > options_.v4_hist)
    v4_history_.pop_front();
  const TradeOffDecisionV4 d =
      DecideTradeOffV4(v4_history_, u_before, options_.v4_umin,
                       options_.v4_umax, options_.v4_median);

  // The snapshot of the decision, for the offline replay.
  rtc::StringBuilder sb;
  sb << "TigasMortiseV4 at_ms=" << at_time.ms() << " u=" << Num(u_before)
     << " cp=" << (cp_detected_ ? 1 : 0) << " mode=" << d.mode
     << " u_pref=" << Num(d.u_pref) << " ratio=" << Num(d.ratio)
     << " q_stand=" << Num(d.q_stand) << " w=" << Num(d.w)
     << " rtt_base=" << rtt_base << " low=" << (low ? 1 : 0)
     << " since_fall=" << std::min(v4_since_fall_, 9999) << " hist_u=";
  for (size_t i = 0; i < v4_history_.size(); ++i)
    sb << (i ? ";" : "") << Num(v4_history_[i].u);
  sb << " hist_b=";
  for (size_t i = 0; i < v4_history_.size(); ++i)
    sb << (i ? ";" : "") << v4_history_[i].bitrate_bps;
  sb << " hist_t=";
  for (size_t i = 0; i < v4_history_.size(); ++i)
    sb << (i ? ";" : "") << v4_history_[i].tput_bps;
  sb << " hist_q=";
  for (size_t i = 0; i < v4_history_.size(); ++i)
    sb << (i ? ";" : "") << Num(v4_history_[i].q_ms);
  sb << " hist_low=";
  for (size_t i = 0; i < v4_history_.size(); ++i)
    sb << (i ? ";" : "") << (v4_history_[i].low ? 1 : 0);
  RTC_LOG(LS_INFO) << sb.str();

  last_trade_off_ = cur_trade_off_;
  std::string branch;
  double u;
  if (cp_detected_) {
    // A change point: go to the target and keep only this period.
    u = d.u_pref;
    while (v4_history_.size() > 1)
      v4_history_.pop_front();
    cp_detected_ = false;
    branch = "v4_cp";
  } else {
    u = options_.v4_sigma * d.u_pref + (1.0 - options_.v4_sigma) * u_before;
    branch = std::string("v4_") + d.mode;
    // The queue of a lower u shows only in a period or two, and a falling
    // least R hides it.
    if (u < u_before && ((options_.v4_hold && low) ||
                         v4_since_fall_ < options_.v4_wait)) {
      u = u_before;
      branch = "v4_hold";
    }
  }
  cur_trade_off_ = std::min(options_.v4_umax, std::max(options_.v4_umin, u));
  if (cur_trade_off_ < u_before)
    v4_since_fall_ = 0;
  else if (v4_since_fall_ < std::numeric_limits<int>::max())
    ++v4_since_fall_;
  Log(branch.c_str(), at_time, u_before, &period);
  return cur_trade_off_ != u_before;
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
    if (period->rtt_min_ms >= 0)
      sb << " rtt_min=" << period->rtt_min_ms;
  }
  RTC_LOG(LS_INFO) << sb.str();
}

}  // namespace webrtc
