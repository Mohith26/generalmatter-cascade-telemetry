# CascadeWorks

An isotope enrichment cascade is a strange machine to reason about. No single
separating stage does much: it nudges the ratio of two isotopes by a small
factor and hands the slightly enriched stream up and the slightly depleted
stream down. Everything interesting comes from wiring hundreds of these
stages together so that streams only ever mix with streams of equal
composition, and from the bookkeeping (separative work) that tells you what
the whole arrangement costs. I wanted to build that bookkeeping from first
principles, prove it against published numbers, and then build the other half
of the picture: the software that would sit next to such a plant and watch
thousands of sensor channels in real time, deciding when something is wrong
and when to shut everything down.

So this repo is two projects joined at the hip:

* `cascade/` is a Python physics core: the Dirac value function, SWU
  accounting, and an ideal (matched abundance ratio) cascade solver, validated
  against worked examples from the open literature.
* `rt/` is a C++17 telemetry and protection layer: a seeded synthetic sensor
  stream generator, a bounded multi producer single consumer queue with
  explicit backpressure accounting, per channel anomaly detection with
  hysteresis, and a cascade wide trip state machine with deterministic replay.

## Scope statement, read this first

Everything physics related here is textbook material from the open
literature: the value function V(x) = (2x - 1) ln(x / (1 - x)), separative
work unit accounting, external stream material balances, and the ideal
cascade stage equations as presented in Benedict, Pigford and Levi, "Nuclear
Chemical Engineering", 2nd ed., McGraw-Hill, 1981, chapter 12. The stage
separation factor is a generic free input parameter. Nothing in this repo
contains or derives real centrifuge design data: no rotor dimensions, speeds,
materials, or actual machine separative performance. The telemetry side is
fully synthetic, with made up generic sensor ranges (a 10,000 rpm nominal, a
temperature around 320, and so on) that describe no real hardware. This is
the same math that appears in nuclear engineering coursework and public IAEA
and World Nuclear Association explainers, nothing more.

## The physics half

`cascade/swu.py` implements the value function and SWU accounting for
arbitrary feed, product, and tails assays, plus the classic tails assay
tradeoff curve (cheaper feed versus cheaper separative work). `cascade/ideal.py`
solves the ideal cascade: given the three assays and a generic stage factor
alpha, it computes enriching and stripping stage counts from the textbook
formulas, then recovers every interstage flow by solving the exact stage by
stage material balance as a tridiagonal linear system. Solving for the flows
instead of assuming them means the tests can verify conservation instead of
trusting it: total and isotope mass balance hold at every stage, every mixing
node, and overall, with a measured worst case relative error around 3e-16
against a 1e-9 requirement, and the assay profile is strictly monotone from
tails to product.

Because integer stage counts are ceilings of the exact textbook expressions,
the achieved product assay meets or slightly exceeds the target and the
achieved tails assay lands at or slightly below it; the solver reports both.

### Checking it against published numbers

I committed the validation examples as fixtures in
`cascade/fixtures/worked_examples.json`, each with its citation. They are:

1. IAEA Bulletin vol. 19 no. 1: 1 kg of 3 percent product at 0.2 percent
   tails takes 5.479 kg of natural uranium and 4.306 SWU.
2. Wikipedia, "Separative work units": 102 kg of natural uranium to 10 kg of
   4.5 percent product at 0.3 percent tails, about 62 SWU.
3. Wikipedia, "Enriched uranium": 1 kg of 3.6 percent product at 0.3 percent
   tails (about 8 kg feed, 4.5 SWU) and at 0.2 percent tails (about 6.7 kg
   feed, nearly 5.7 SWU).
4. World Nuclear Association, "Uranium Enrichment": 1 kg of 5 percent product
   at 0.25 percent tails (7.9 SWU, 10.4 kg feed) and at 0.20 percent tails
   (8.9 SWU, 9.4 kg feed).

All six cases reproduce within 1 percent relative error, and the tightest
(the IAEA one, quoted to four figures) matches to about 1e-4. The looser ones
are loose because the sources rounded; the fixture tolerances record that.
`python -m cascade.validate` regenerates `results/physics_validation.json`
from scratch.

## The real time half

The C++ side simulates the plant floor view: N channels, four sensors each
(rpm, vibration, temperature, current), one frame per channel per tick.
Producer threads own disjoint channel shards and push frame batches into a
bounded queue; a single consumer runs detection and the protection state
machine. Four fault types get injected with known ground truth: overspeed
ramps, vibration spikes, thermal drift, and sensor dropout.

Detection is per channel with hysteresis (a streak of bad frames to raise, a
streak of good ones to clear): plain thresholds for overspeed and vibration,
a rolling rate of change window for thermal drift, and a tick watermark gap
scan for dropout. Channel alarms feed a cascade wide state machine, NORMAL to
ALARM to TRIP: a persistent single channel alarm or two simultaneously
alarmed channels trips the cascade, which quiesces every channel, then auto
resets after a cooldown so long runs can exercise many episodes.

The part I care most about is determinism. Every sensor sample is a pure
function of (seed, channel, tick), each channel lives on exactly one
producer so its frame order is preserved, and all global decisions happen at
tick finalization on canonically sorted events. The event log therefore
hashes identically across runs regardless of thread interleaving, which the
replay harness and tests verify. Wall clock latency is measured but never
hashed.

## What the numbers say

All measured on my machine: Apple silicon (arm64), 18 logical cores, Apple
clang, `-std=c++17 -O2`, 6 producer threads plus 1 consumer. Different
hardware will give different numbers. Raw JSON lives in `results/`.

* Unpaced throughput, zero loss verified (accepted == emitted == processed,
  0 dropped): about 59.0M frames/s at 1,000 channels, 91.5M at 5,000, and
  100.9M at 10,000 (larger per tick batches amortize queue locking, which is
  why more channels goes faster).
* At the rated operating point (100 Hz per channel, so 1M frames/s at 10,000
  channels), ingest to detection latency was p50 88 us and p99 268 us, with
  zero loss. At 1,000 channels it was p50 18 us, p99 82 us.
* Detection quality over 48 labeled fault episodes (12 per type, 64 channels,
  20,000 ticks): precision 1.0 on every type; recall 1.0 for overspeed,
  thermal drift, and dropout; recall 0.83 for vibration spikes. The two
  missed spikes had injected amplitudes below the alarm threshold, which the
  generator produces on purpose; I kept the miss rather than tuning the
  threshold down until the eval looked perfect.
* Detection to trip latency was p50 10 ticks and p99 11.3 ticks (100 ms and
  113 ms at the nominal 10 ms tick), dominated by the deliberate 10 tick
  persistence hysteresis.
* Replay: two runs at the same seed produced byte identical event logs (328
  events, matching FNV-1a hashes); a different seed produced a different log.

One honest quirk: the run recorded 58 trips against 46 detected episodes,
because after the auto reset a still active alarm legitimately re trips the
cascade. That is the configured behavior, not a bug, but it does inflate the
trip count on long episodes.

## Tests

92 in total: 61 pytest cases over the physics core (worked examples,
conservation invariants, solver edge cases like a single stage cascade,
product assay barely above feed, and tails near zero) and 31 C++ assert
harness cases (queue contention and backpressure accounting, alarm hysteresis,
state machine transitions, quiesce coverage, replay hash equality, and the
episode scoring logic itself).

## Running it

```
make venv    # python3 -m venv .venv && pip install numpy pytest
make all     # builds build/rt and build/rt_tests (clang++, no external deps)
make test    # C++ harness, then pytest
make bench   # regenerates everything in results/
```

## Limitations

The plant is open loop: a trip is recorded and quiesce is tracked per
channel, but nothing feeds back into the generator, so faults keep going
until their episode ends. The ideal cascade is the textbook idealization,
not a design tool: real cascades are squared off, not ideal, and none of
that is modeled here. Telemetry is synthetic and seeded, so detection scores
say nothing about real sensors. The thermal window measures frames ago, not
ticks ago, so a dropout gap stretches its effective span. Latency numbers
are stamped once per dequeued batch, and all performance figures are
specific to the machine above.
