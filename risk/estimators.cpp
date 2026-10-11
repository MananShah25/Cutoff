#include "estimators.hpp"

namespace cutoff::risk {

const char* estimator_name(Estimator e) {
  switch (e) {
    case Estimator::GoodTuring: return "good-turing";
    case Estimator::IncidenceRate: return "incidence";
    case Estimator::CoverageRisk: return "coverage";
  }
  return "?";
}

double good_turing(const Counts& c) {
  if (c.n == 0) return 1.0;
  return static_cast<double>(c.q1) / static_cast<double>(c.n);
}

double incidence_rate(const Counts& c) {
  if (c.v == 0) return 1.0;
  return static_cast<double>(c.q1) / static_cast<double>(c.v);
}

double chao2_classic(const Counts& c) {
  const double s = static_cast<double>(c.s);
  const double q1 = static_cast<double>(c.q1);
  const double q2 = static_cast<double>(c.q2);
  if (c.q2 > 0) return s + q1 * q1 / (2.0 * q2);
  return s + q1 * (q1 - 1.0) / 2.0;
}

double chao2_bias_corrected(const Counts& c) {
  if (c.n == 0) return static_cast<double>(c.s);
  const double q1 = static_cast<double>(c.q1);
  const double q2 = static_cast<double>(c.q2);
  const double n = static_cast<double>(c.n);
  return static_cast<double>(c.s) + (n - 1.0) / n * q1 * (q1 - 1.0) / (2.0 * (q2 + 1.0));
}

double coverage_risk(const Counts& c) {
  if (c.v == 0 || c.n == 0) return 1.0;
  if (c.q1 == 0) return 0.0;
  const double q0 = chao2_classic(c) - static_cast<double>(c.s);
  const double n = static_cast<double>(c.n);
  const double q1 = static_cast<double>(c.q1);
  return q1 / static_cast<double>(c.v) * (n * q0) / (n * q0 + q1);
}

double risk_value(const Counts& c, Estimator e) {
  switch (e) {
    case Estimator::GoodTuring: return good_turing(c);
    case Estimator::IncidenceRate: return incidence_rate(c);
    case Estimator::CoverageRisk: return coverage_risk(c);
  }
  return 1.0;
}

}  // namespace cutoff::risk
