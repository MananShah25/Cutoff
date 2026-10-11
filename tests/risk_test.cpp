// Tests for species definitions, the incremental tracker, the estimators (hand-computed values and simulations on a
// known occupancy model) and the stop rule.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

#include "estimators.hpp"
#include "model.hpp"
#include "rng.hpp"
#include "rv32i.hpp"
#include "species.hpp"
#include "stop_rule.hpp"
#include "tracker.hpp"

using namespace cutoff;
using namespace cutoff::risk;

// ---------------------------------------------------------------- species definitions

TEST(Species, OperandClassesFollowTheDocumentedPriorityOrder) {
  struct C { std::uint32_t v; OperandClass c; };
  const C table[] = {
      {0, OperandClass::Zero},          {1, OperandClass::One},           {0xffffffff, OperandClass::MinusOne},
      {0x80000000, OperandClass::IntMin}, {0x7fffffff, OperandClass::IntMax}, {2, OperandClass::SmallPositive},
      {255, OperandClass::SmallPositive}, {256, OperandClass::Other},      {0xfffffffe, OperandClass::SmallNegative},
      {0xffffff01, OperandClass::SmallNegative}, {0xffffff00, OperandClass::Other}, {0x12345678, OperandClass::Other},
      {0x80000001, OperandClass::Other}, {0x7ffffffe, OperandClass::Other},
  };
  for (const auto& t : table) EXPECT_EQ(classify(t.v), t.c) << std::hex << t.v;
  EXPECT_STREQ(class_name(OperandClass::IntMin), "int-min");
}

TEST(Species, CrossIdsEncodeKindAndBothOperandClasses) {
  constexpr unsigned add = static_cast<unsigned>(Op::ADD), addi = static_cast<unsigned>(Op::ADDI), lui = static_cast<unsigned>(Op::LUI);
  EXPECT_EQ(cross_species(Op::ADD, 1, 0xffffffff), add * 64 + 1 * 8 + 2);
  EXPECT_EQ(cross_species(Op::ADD, 0, 0), add * 64);
  // An operand the kind does not read always has class 0, whatever the junk value says.
  EXPECT_EQ(cross_species(Op::ADDI, 0x12345678, 0xffffffff), addi * 64 + static_cast<unsigned>(OperandClass::Other) * 8);
  EXPECT_EQ(cross_species(Op::LUI, 0xdead, 0xbeef), lui * 64);
  // Every possible id fits the declared universe.
  for (unsigned op = 0; op < kNumOps; ++op)
    for (const std::uint32_t v1 : {0u, 1u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 7u, 0xfffffff0u, 0x1234u})
      EXPECT_LT(cross_species(static_cast<Op>(op), v1, v1), universe_size(SpeciesSet::Cross));
  EXPECT_EQ(universe_size(SpeciesSet::Op), kNumOps);
}

TEST(Species, ExtractionFromARetireTraceIsDeduplicatedAndSorted) {
  const std::vector<std::uint32_t> program = {rv::addi(1, 0, 1), rv::addi(2, 0, -1), rv::add(3, 1, 2), rv::add(4, 1, 2),
                                              rv::addi(5, 0, 1), rv::lui(6, 0x12345)};
  const ModelResult r = run_model(program, {100, true});  // ends in a Trap on the zero word; the trace has 6 retires
  ASSERT_EQ(r.retired.size(), 6u);
  const SpeciesIds ids = extract_species(r.retired, SpeciesConfig{});
  const auto& op = ids[static_cast<std::size_t>(SpeciesSet::Op)];
  const auto& cross = ids[static_cast<std::size_t>(SpeciesSet::Cross)];
  EXPECT_EQ(op, (std::vector<std::uint16_t>{static_cast<std::uint16_t>(Op::LUI), static_cast<std::uint16_t>(Op::ADDI), static_cast<std::uint16_t>(Op::ADD)}));
  // addi x?,x0,imm: rs1 = x0 = zero -> one cell (class pair 0/0). add x?,x1,x2: x1 = 1 (one), x2 = -1 (minus one).
  const std::uint16_t lui_cell = static_cast<std::uint16_t>(Op::LUI) * 64;
  const std::uint16_t addi_cell = static_cast<std::uint16_t>(Op::ADDI) * 64;
  const std::uint16_t add_cell = static_cast<std::uint16_t>(Op::ADD) * 64 + 1 * 8 + 2;
  EXPECT_EQ(cross, (std::vector<std::uint16_t>{lui_cell, addi_cell, add_cell}));
  EXPECT_TRUE(std::is_sorted(cross.begin(), cross.end()));
  EXPECT_TRUE(ids[2].empty() && ids[3].empty());

  SpeciesConfig only_cross;
  only_cross.op = false;
  const SpeciesIds ids2 = extract_species(r.retired, only_cross);
  EXPECT_TRUE(ids2[0].empty());
  EXPECT_EQ(ids2[1], cross);
}

TEST(Species, TheTraceCarriesOperandValuesOnlyForKindsThatReadThem) {
  const std::vector<std::uint32_t> program = {rv::lui(1, 0x80000), rv::addi(2, 1, -1), rv::sub(3, 1, 2), rv::sw(3, 0, 0x100), rv::jal(0, 4)};
  const ModelResult r = run_model(program, {5, true});
  ASSERT_GE(r.retired.size(), 5u);
  EXPECT_EQ(r.retired[0].rs1_value, 0u);                 // lui reads nothing
  EXPECT_EQ(r.retired[1].rs1_value, 0x80000000u);        // addi x2,x1,-1
  EXPECT_EQ(r.retired[1].rs2_value, 0u);                 // addi does not read rs2
  EXPECT_EQ(r.retired[2].rs1_value, 0x80000000u);        // sub x3,x1,x2: x2 = 0x7fffffff
  EXPECT_EQ(r.retired[2].rs2_value, 0x7fffffffu);
  EXPECT_EQ(r.retired[3].rs2_value, r.regs[3]);          // sw stores x3
  EXPECT_EQ(r.retired[4].rs1_value, 0u);                 // jal reads nothing
}

TEST(Species, ReadsHelpersMatchTheInstructionFormats) {
  for (const Op op : {Op::LUI, Op::AUIPC, Op::JAL, Op::FENCE}) EXPECT_FALSE(reads_rs1(op)) << op_name(op);
  for (const Op op : {Op::JALR, Op::BEQ, Op::LW, Op::SB, Op::ADDI, Op::SRAI, Op::ADD, Op::AND}) EXPECT_TRUE(reads_rs1(op)) << op_name(op);
  for (const Op op : {Op::BEQ, Op::BGEU, Op::SB, Op::SW, Op::ADD, Op::SRA}) EXPECT_TRUE(reads_rs2(op)) << op_name(op);
  for (const Op op : {Op::ADDI, Op::LW, Op::JALR, Op::LUI, Op::SLLI}) EXPECT_FALSE(reads_rs2(op)) << op_name(op);
}

// ---------------------------------------------------------------- tracker

TEST(Tracker, HandComputedCountsAfterEachProgram) {
  SpeciesTracker t(10);
  EXPECT_TRUE(t.add_program(std::vector<std::uint16_t>{0, 1, 2}));
  EXPECT_EQ(t.counts().n, 1u);
  EXPECT_EQ(t.counts().s, 3u);
  EXPECT_EQ(t.counts().q1, 3u);
  EXPECT_EQ(t.counts().v, 3u);
  EXPECT_EQ(t.last_new_n(), 1u);
  EXPECT_TRUE(t.add_program(std::vector<std::uint16_t>{1, 2}));
  EXPECT_EQ(t.counts().q1, 1u);   // species 0 only
  EXPECT_EQ(t.counts().q2, 2u);   // species 1 and 2
  EXPECT_EQ(t.counts().s, 3u);
  EXPECT_EQ(t.programs_since_new(), 1u);
  EXPECT_TRUE(t.add_program(std::vector<std::uint16_t>{2, 5}));
  EXPECT_EQ(t.counts().n, 3u);
  EXPECT_EQ(t.counts().s, 4u);
  EXPECT_EQ(t.counts().q1, 2u);   // species 0 and 5
  EXPECT_EQ(t.counts().q2, 1u);   // species 1
  EXPECT_EQ(t.counts().v, 7u);    // 1 + 2 + 3 + 1
  EXPECT_EQ(t.last_new_n(), 3u);
  EXPECT_EQ(t.programs_since_new(), 0u);
  EXPECT_TRUE(t.add_program(std::vector<std::uint16_t>{}));  // a program may exercise nothing new (or nothing)
  EXPECT_EQ(t.counts().n, 4u);
  EXPECT_EQ(t.programs_since_new(), 1u);
}

TEST(Tracker, RejectsMalformedProgramsWithoutChangingState) {
  SpeciesTracker t(10);
  ASSERT_TRUE(t.add_program(std::vector<std::uint16_t>{1, 3}));
  const Counts before = t.counts();
  EXPECT_FALSE(t.add_program(std::vector<std::uint16_t>{3, 3}));      // duplicate
  EXPECT_FALSE(t.add_program(std::vector<std::uint16_t>{4, 2}));      // unsorted
  EXPECT_FALSE(t.add_program(std::vector<std::uint16_t>{1, 10}));     // outside the universe
  EXPECT_EQ(t.counts().n, before.n);
  EXPECT_EQ(t.counts().v, before.v);
  EXPECT_EQ(t.per_species()[3], 1u);
}

TEST(Tracker, MatchesBruteForceOnRandomStreamsAndSurvivesSaveRestore) {
  gen::Rng rng(31337);
  SpeciesTracker t(60);
  for (int program = 1; program <= 400; ++program) {
    std::vector<std::uint16_t> ids;
    for (std::uint16_t s = 0; s < 60; ++s)
      if (rng.below(100) < 1 + 2 * (s % 25)) ids.push_back(s);
    ASSERT_TRUE(t.add_program(ids));
    Counts brute;
    brute.n = static_cast<std::uint64_t>(program);
    for (const std::uint32_t y : t.per_species()) {
      if (y > 0) ++brute.s;
      if (y == 1) ++brute.q1;
      if (y == 2) ++brute.q2;
      brute.v += y;
    }
    ASSERT_EQ(t.counts().s, brute.s);
    ASSERT_EQ(t.counts().q1, brute.q1);
    ASSERT_EQ(t.counts().q2, brute.q2);
    ASSERT_EQ(t.counts().v, brute.v);
    if (program % 100 == 0) {
      const SpeciesTracker r = SpeciesTracker::restore(t.per_species(), t.counts().n, t.last_new_n());
      ASSERT_EQ(r.counts().s, t.counts().s);
      ASSERT_EQ(r.counts().q1, t.counts().q1);
      ASSERT_EQ(r.counts().q2, t.counts().q2);
      ASSERT_EQ(r.counts().v, t.counts().v);
      ASSERT_EQ(r.counts().n, t.counts().n);
      ASSERT_EQ(r.programs_since_new(), t.programs_since_new());
    }
  }
}

TEST(Tracker, EstimatesAtProgramNOnlyDependOnTheFirstNPrograms) {
  // "No estimator reads outcomes of future programs": a tracker fed the first n programs of a longer stream has
  // exactly the same state as the long one had after n programs.
  gen::Rng rng(5);
  std::vector<std::vector<std::uint16_t>> stream;
  for (int p = 0; p < 300; ++p) {
    std::vector<std::uint16_t> ids;
    for (std::uint16_t s = 0; s < 40; ++s)
      if (rng.below(100) < 3 + s) ids.push_back(s);
    stream.push_back(ids);
  }
  SpeciesTracker long_run(40);
  std::vector<Counts> history;
  for (const auto& p : stream) {
    long_run.add_program(p);
    history.push_back(long_run.counts());
  }
  for (const std::size_t n : {10u, 100u, 299u}) {
    SpeciesTracker prefix(40);
    for (std::size_t i = 0; i < n; ++i) prefix.add_program(stream[i]);
    const Counts& want = history[n - 1];
    EXPECT_EQ(prefix.counts().q1, want.q1);
    EXPECT_EQ(prefix.counts().q2, want.q2);
    EXPECT_EQ(prefix.counts().v, want.v);
    for (const Estimator e : {Estimator::GoodTuring, Estimator::IncidenceRate, Estimator::CoverageRisk})
      EXPECT_EQ(risk_value(prefix.counts(), e), risk_value(want, e));
  }
}

// ---------------------------------------------------------------- estimators with hand-computed values

TEST(Estimators, HandComputedValues) {
  const Counts c{10, 20, 4, 2, 60};
  EXPECT_DOUBLE_EQ(good_turing(c), 0.4);                           // Q1 / n
  EXPECT_DOUBLE_EQ(incidence_rate(c), 4.0 / 60.0);                 // Q1 / V
  EXPECT_DOUBLE_EQ(chao2_classic(c), 24.0);                        // 20 + 16 / (2*2)
  EXPECT_NEAR(chao2_bias_corrected(c), 21.8, 1e-12);               // 20 + 0.9 * 4*3 / (2*3)
  // S30 Eq. 28: (Q1/V) * n*Q0 / (n*Q0 + Q1) with Q0 = 24 - 20 = 4  ->  (4/60) * 40/44
  EXPECT_NEAR(coverage_risk(c), (4.0 / 60.0) * (40.0 / 44.0), 1e-15);
  EXPECT_NEAR(coverage_risk(c), 0.0606060606060606, 1e-12);

  const Counts no_doubletons{5, 3, 2, 0, 7};
  EXPECT_DOUBLE_EQ(chao2_classic(no_doubletons), 3.0 + 2.0 * 1.0 / 2.0);   // Q2 = 0 form of S30 Eq. 26
  EXPECT_NEAR(chao2_bias_corrected(no_doubletons), 3.0 + 0.8 * 2.0 * 1.0 / 2.0, 1e-12);
}

TEST(Estimators, EdgeCases) {
  EXPECT_DOUBLE_EQ(good_turing(Counts{}), 1.0) << "no data means maximum risk, never zero";
  EXPECT_DOUBLE_EQ(incidence_rate(Counts{}), 1.0);
  EXPECT_DOUBLE_EQ(coverage_risk(Counts{}), 1.0);
  const Counts all_repeated{50, 10, 0, 0, 500};  // every species seen many times
  EXPECT_DOUBLE_EQ(good_turing(all_repeated), 0.0);
  EXPECT_DOUBLE_EQ(incidence_rate(all_repeated), 0.0);
  EXPECT_DOUBLE_EQ(coverage_risk(all_repeated), 0.0);
  EXPECT_DOUBLE_EQ(chao2_classic(all_repeated), 10.0);
  EXPECT_DOUBLE_EQ(chao2_bias_corrected(all_repeated), 10.0);
  const Counts one_program{1, 7, 7, 0, 7};  // after a single program every species is a singleton
  EXPECT_DOUBLE_EQ(good_turing(one_program), 7.0) << "Q1/n is a rate and can exceed 1 early on";
  EXPECT_DOUBLE_EQ(incidence_rate(one_program), 1.0);
  EXPECT_DOUBLE_EQ(risk_value(Counts{10, 20, 4, 2, 60}, Estimator::GoodTuring), 0.4);
  EXPECT_DOUBLE_EQ(risk_value(Counts{10, 20, 4, 2, 60}, Estimator::IncidenceRate), 4.0 / 60.0);
  EXPECT_STREQ(estimator_name(Estimator::CoverageRisk), "coverage");
}

// ---------------------------------------------------------------- simulation on a known occupancy model

namespace {

constexpr int kSpecies = 300;

std::vector<double> species_probabilities() {
  std::vector<double> p(kSpecies);
  for (int i = 0; i < kSpecies; ++i) p[i] = 0.6 / std::pow(1.0 + i, 0.8);  // a few common species, a long rare tail
  return p;
}

struct Mean {
  double sum = 0, sumsq = 0;
  int n = 0;
  void add(double x) { sum += x; sumsq += x * x; ++n; }
  double mean() const { return sum / n; }
  double se() const { return std::sqrt(std::max(0.0, sumsq / n - mean() * mean()) / n); }
};

struct Row {
  Mean gt, incidence, coverage, p_next_new, rate_next, s_obs, chao_classic, chao_bc, v_over_n;
};

}  // namespace

TEST(EstimatorSimulation, GoodTuringTracksItsTheoryAndBoundsTheNextProgramDiscoveryProbability) {
  const std::vector<double> p = species_probabilities();
  const std::vector<int> checkpoints = {25, 50, 100, 200, 400};
  const int reps = 400;
  std::vector<Row> rows(checkpoints.size());
  std::vector<std::uint32_t> thresholds(kSpecies);
  for (int i = 0; i < kSpecies; ++i) thresholds[i] = static_cast<std::uint32_t>(p[i] * 4294967296.0);

  for (int rep = 0; rep < reps; ++rep) {
    gen::Rng rng(gen::program_seed(77, static_cast<std::uint64_t>(rep)));
    SpeciesTracker tracker(kSpecies);
    std::size_t next_cp = 0;
    for (int n = 1; n <= checkpoints.back(); ++n) {
      std::vector<std::uint16_t> ids;
      for (int i = 0; i < kSpecies; ++i)
        if (rng.next32() < thresholds[i]) ids.push_back(static_cast<std::uint16_t>(i));
      tracker.add_program(ids);
      if (n != checkpoints[next_cp]) continue;
      Row& row = rows[next_cp++];
      const Counts& c = tracker.counts();
      double rate = 0, log_none_new = 0;  // truth, given which species this replication has actually seen
      for (int i = 0; i < kSpecies; ++i)
        if (tracker.per_species()[i] == 0) {
          rate += p[i];
          log_none_new += std::log1p(-p[i]);
        }
      row.gt.add(good_turing(c));
      row.incidence.add(incidence_rate(c));
      row.coverage.add(coverage_risk(c));
      row.rate_next.add(rate);
      row.p_next_new.add(1.0 - std::exp(log_none_new));
      row.s_obs.add(static_cast<double>(c.s));
      row.chao_classic.add(chao2_classic(c));
      row.chao_bc.add(chao2_bias_corrected(c));
      row.v_over_n.add(static_cast<double>(c.v) / static_cast<double>(c.n));
      EXPECT_GE(chao2_classic(c), static_cast<double>(c.s));
      EXPECT_LE(coverage_risk(c), incidence_rate(c) + 1e-15);
    }
  }

  std::printf("[EstimatorSimulation] %d species, %d replications; means over replications\n", kSpecies, reps);
  std::printf("[EstimatorSimulation]   n   Q1/n   P(next new)  E[new]   Q1/V   coverage  V/n   S_obs  chao2  chao2bc\n");
  for (std::size_t k = 0; k < checkpoints.size(); ++k) {
    const Row& r = rows[k];
    std::printf("[EstimatorSimulation] %4d  %.4f  %.4f      %.4f   %.4f  %.4f   %.2f  %.1f  %.1f  %.1f\n", checkpoints[k], r.gt.mean(),
                r.p_next_new.mean(), r.rate_next.mean(), r.incidence.mean(), r.coverage.mean(), r.v_over_n.mean(), r.s_obs.mean(),
                r.chao_classic.mean(), r.chao_bc.mean());
  }

  for (std::size_t k = 0; k < checkpoints.size(); ++k) {
    const int n = checkpoints[k];
    const Row& r = rows[k];
    // Theory: E[Q1(n)] / n = sum p (1-p)^(n-1) and E[expected new species at n+1] = sum p (1-p)^n.
    double e_gt = 0, e_rate = 0;
    for (const double pi : p) {
      e_gt += pi * std::pow(1.0 - pi, n - 1);
      e_rate += pi * std::pow(1.0 - pi, n);
    }
    EXPECT_NEAR(r.gt.mean(), e_gt, 4 * r.gt.se() + 1e-9) << "Q1/n must match its theoretical expectation, n=" << n;
    EXPECT_NEAR(r.rate_next.mean(), e_rate, 4 * r.rate_next.se() + 1e-9) << "n=" << n;
    // Good-Turing is conservative: it is not below the expected number of new species, hence not below the
    // probability that the next program shows at least one.
    EXPECT_GE(r.gt.mean() + 3 * r.gt.se(), r.rate_next.mean()) << "n=" << n;
    EXPECT_GE(r.gt.mean() + 3 * r.gt.se(), r.p_next_new.mean()) << "n=" << n;
    EXPECT_LE(r.rate_next.mean(), r.gt.mean() * 1.3 + 3 * r.gt.se()) << "and not wildly conservative, n=" << n;
  }

  // S30's Q1/V is a per-occurrence share, smaller than Q1/n by about V/n (the mean species per program). Used as
  // the stop signal it would claim the risk is several times lower than it is.
  const Row& r200 = rows[3];
  EXPECT_GT(r200.v_over_n.mean(), 3.0);
  EXPECT_LT(r200.incidence.mean(), r200.p_next_new.mean() / 3.0) << "Q1/V understates the chance that the next program finds something new";
  EXPECT_NEAR(r200.gt.mean() / r200.incidence.mean(), r200.v_over_n.mean(), 0.2 * r200.v_over_n.mean());
  EXPECT_LT(r200.coverage.mean(), r200.incidence.mean());

  // Richness: the classic Chao2 is a lower bound on S; the bias-corrected form is closer to the truth than S_obs.
  for (std::size_t k = 0; k < checkpoints.size(); ++k) {
    EXPECT_LE(rows[k].chao_classic.mean() - 3 * rows[k].chao_classic.se(), static_cast<double>(kSpecies)) << "n=" << checkpoints[k];
    EXPECT_LT(std::abs(rows[k].chao_bc.mean() - kSpecies), std::abs(rows[k].s_obs.mean() - kSpecies)) << "n=" << checkpoints[k];
  }
}

// ---------------------------------------------------------------- stop rule

TEST(StopRule, NeedsConfirmConsecutiveCheckpointsAfterMinPrograms) {
  StopConfig cfg;
  cfg.epsilon = 0.1;
  cfg.confirm = 3;
  cfg.min_programs = 100;
  cfg.checkpoint = 50;
  StopRule r(cfg);
  EXPECT_TRUE(r.enabled());
  EXPECT_TRUE(r.at_checkpoint(150));
  EXPECT_FALSE(r.at_checkpoint(120));
  r.on_checkpoint(50, 0.01);
  EXPECT_EQ(r.consecutive(), 0u) << "checkpoints before min_programs do not count";
  r.on_checkpoint(100, 0.05);
  EXPECT_EQ(r.consecutive(), 1u);
  r.on_checkpoint(150, 0.2);
  EXPECT_EQ(r.consecutive(), 0u) << "a risk at or above epsilon resets the count";
  r.on_checkpoint(200, 0.1);
  EXPECT_EQ(r.consecutive(), 0u) << "the comparison is strict: risk == epsilon is not below it";
  r.on_checkpoint(250, 0.05);
  r.on_checkpoint(300, 0.05);
  EXPECT_FALSE(r.should_stop());
  r.on_checkpoint(350, 0.05);
  EXPECT_TRUE(r.should_stop());
  StopRule resumed(cfg);
  resumed.restore(2);
  resumed.on_checkpoint(400, 0.01);
  EXPECT_TRUE(resumed.should_stop());
}

TEST(StopRule, ZeroEpsilonSwitchesItOffAndNoDataIsNeverSafe) {
  StopConfig off;
  StopRule r(off);
  EXPECT_FALSE(r.enabled());
  for (std::uint64_t n = 1000; n < 20000; n += 1000) r.on_checkpoint(n, 0.0);
  EXPECT_FALSE(r.should_stop());
  EXPECT_EQ(r.consecutive(), 0u);
  EXPECT_DOUBLE_EQ(good_turing(Counts{}), 1.0) << "an estimate of zero after no programs must not be read as safety";
}
