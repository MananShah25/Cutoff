#pragma once
// Species-discovery estimators on incidence data (a program is a sampling unit; a species is either exercised by
// it or not). Counts: n programs, S species seen, Q1 species seen by exactly one program (singletons), Q2 by exactly
// two (doubletons), V = sum over species of the number of programs that exercised it.
//
// Sources (read in full, see docs/verification_log.md V-32):
//   good_turing        Q1/n               S31 Eq. 22 ("Good-Turing", probability that the next input discovers a new
//                                         species) and S32 (where Q1/n is called the coverage rate: an upper bound on
//                                         the discovery probability, known to over-estimate it)
//   incidence_rate     Q1/V               S30 Eq. 29 (Bernoulli product model): the share of species occurrences that are
//                                         singletons. A different quantity: it is smaller than good_turing by V/n, the
//                                         mean number of species per program, so it must NOT be used as the stop signal
//   coverage_risk      Eq. 28 of S30      (Q1/V) * n*Q0/(n*Q0 + Q1), Q0 = chao2_classic - S: unseen share of all occurrences
//   chao2_classic      S30 Eq. 26         S + Q1^2/(2 Q2) if Q2 > 0, else S + Q1 (Q1-1)/2
//   chao2_bias_corrected                  S + (n-1)/n * Q1 (Q1-1) / (2 (Q2+1))  (standard bias-corrected Chao2; not in S30,
//                                         checked here only against simulation)

#include <cstdint>

namespace cutoff::risk {

struct Counts {
  std::uint64_t n = 0;   // programs observed
  std::uint64_t s = 0;   // species seen at least once
  std::uint64_t q1 = 0;
  std::uint64_t q2 = 0;
  std::uint64_t v = 0;   // total species occurrences
};

enum class Estimator : std::uint8_t { GoodTuring, IncidenceRate, CoverageRisk };

const char* estimator_name(Estimator e);  // "good-turing", "incidence", "coverage"

// All estimators return 1.0 (maximum risk) when there is no data to base them on (n == 0 or v == 0).
double good_turing(const Counts& c);
double incidence_rate(const Counts& c);
double chao2_classic(const Counts& c);          // estimated richness (total species), not a risk
double chao2_bias_corrected(const Counts& c);
double coverage_risk(const Counts& c);

double risk_value(const Counts& c, Estimator e);

}  // namespace cutoff::risk
