# Literature check (spec section 16), 2026-10-11

Done at the start of Step 5, before any estimator code. Method: web search (standard and extended modes) plus fetching
pages; full texts of S30 and S31 were extracted locally and read in the sections that matter. Limits: paywalled databases
(IEEE Xplore, ACM DL beyond open PDFs, Springer) were not searched, many hardware-verification papers are only reachable
through them, and search snippets and abstracts are weaker evidence than full texts. "Not found" below means not found by
these searches, not "does not exist".

## What was read, and how fully

| Source | Read | Used for |
| --- | --- | --- |
| S30 STADS (Böhme) | Full text extracted (about 33,000 words); read Sections 1-3, the multinomial estimators, Section 7 (Bernoulli product / incidence model), 9.4-9.5 (assumptions, adaptive bias) | Definitions of Q_k, Good-Turing, Chao2, Eq. 26-31 |
| S31 Estimating residual risk in greybox fuzzing (Böhme, Liyanage, Wüstholz) | Full text extracted; read Sections 2, 3.2 and 4 | Definition of residual risk and discovery probability, Good-Turing as f1/n for inputs that exercise many species |
| S32 Dependency-aware residual risk analysis (Lee, Böhme, ICSE 2026) | **Abstract only** (conference page); the preprint at nimgnoeseel.github.io/assets/pdf/structrr-paper.pdf was not read | That Good-Turing (f1/n) estimates the *coverage rate*, which bounds the discovery probability, and that it over-estimates |
| S34 When to stop fuzzing (Fuzzing Book) | First 100,000 of 270,000 characters | Good-Turing p0 = f1/n and sample coverage 1 - GT |
| S33, S35, S36 | Search results and abstracts only | Context |

## Finding that changed the implementation

S30 gives two sampling models. In the multinomial model (one species per input) the discovery probability is f1/n. In the
Bernoulli product / incidence model (an input exercises many species, which is Cutoff's case) S30 Eq. 29 gives **Q1/V**,
with V the total number of species occurrences, a different and much smaller quantity. S31 uses f1/n for multi-species inputs
and interprets it as the probability that the next input discovers a new species, and S32's abstract calls f1/n the
coverage rate (an upper bound on the discovery probability) and reports that it over-estimates. The spec's "Q1/n from S30"
therefore follows S31/S32, not S30's incidence section. Decision D-025; simulation V-32 shows Q1/V understating the risk of
a new behaviour by about 6 times. The spec's [knowledge] item "exact forms of the Chao-type estimators" is resolved for the
classic Chao2 (S30 Eq. 26) and the Eq. 28 coverage form; the bias-corrected Chao2 is the standard literature form, not in S30.

## Closest prior work found

No source applying residual-risk, Good-Turing or species-richness estimators to RTL simulation, processor verification,
constrained-random regression or RISC-V test generation was found. Nearby work:

| Work | Why it is close | How it differs |
| --- | --- | --- |
| [Bayesian based stopping rules for behavioral VHDL verification](https://mountainscholar.org/items/d30644b2-aa85-42ab-a1d4-64f262143a90) (Colorado State, about 2001; known from search results, not read) | A statistical stopping rule for hardware simulation with coverage forecasting | Bayesian coverage model, behavioural VHDL, no CPU, no planted-bug validation |
| [DVCon: gray-box reachability predictor for constrained-random verification](https://dvcon-proceedings.org/wp-content/uploads/1065-2.pdf) (abstract via search) | Reports 32% time saving against CRV saturation time | Chooses which tests to run; not a stopping estimate |
| ChipFuzzer (found through a search summary, not read) | Stops a stage when coverage gain over 100 test cases is below 0.1% | A fixed heuristic threshold, closest to the plateau baseline Cutoff also records |
| [Green Fuzzing (ISSTA 2023)](https://mboehme.github.io/paper/ISSTA23.pdf) | Saturation-based stopping criterion | Software; vulnerability-prediction-weighted coverage saturation |
| [Estimating Residual Risk in Greybox Fuzzing](https://mboehme.github.io/paper/FSE21.pdf) and [Dependency-aware Residual Risk Analysis](https://mboehme.github.io/paper/ICSE26-dependency.pdf) | The estimators Cutoff uses and its improvement | Software fuzzing on FuzzBench |
| [Assessing Reliability of Statistical Maximum Coverage Estimators in Fuzzing](https://arxiv.org/abs/2507.17093) (2025 registered report) | Evaluates species-richness estimators against ground truth | Software only |
| [Risk Estimation in Differential Fuzzing via Extreme Value Theory](https://arxiv.org/abs/2511.02927) (ASE 2025) | Differential fuzzing (as Cutoff's model-versus-core check) with a risk-based early stop | Java libraries and side channels, extreme-value model of output differences |
| [Confidence intervals for maximum unseen probabilities](https://arxiv.org/abs/2601.20320) (2026) | Distribution-free stopping rules for a Bernoulli product model | Statistics paper, no hardware application; a candidate alternative stop rule |

## Conclusion for the project

Nothing found contradicts the positioning in the spec: describe what Cutoff does (a CPU-specific stop decision on
operand-aware species, validated by planted bugs in the same tool) and do not claim it is the first. The hardware-side
neighbours stop on heuristics or Bayesian coverage forecasts, and the closest statistical work is software fuzzing. The
risks this check adds: Good-Turing on dependent species over-estimates (S32), which Cutoff's SP-cross cells (structurally
dependent through shared operands) are likely to exhibit; the benchmark in Steps 10-11 must measure that calibration
against planted bugs before any claim is made.
