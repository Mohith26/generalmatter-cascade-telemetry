"""Run the physics validation suite and write results/physics_validation.json.

Usage: .venv/bin/python -m cascade.validate

Checks every published worked example fixture against the code, solves a
sample ideal cascade, and records the measured conservation errors. Nothing
in the output file is hand written; it is regenerated on every run.
"""

import json
import os
import platform
import sys
from datetime import datetime, timezone

from . import (
    material_balance,
    separative_work,
    solve_ideal_cascade,
    check_invariants,
    tails_tradeoff,
)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run_fixture(example):
    inp = example["inputs"]
    feed_kg, tails_kg = material_balance(
        inp["product_kg"], inp["feed_assay"], inp["product_assay"], inp["tails_assay"]
    )
    swu = separative_work(
        inp["product_kg"], inp["feed_assay"], inp["product_assay"], inp["tails_assay"]
    )
    computed = {"feed_kg": feed_kg, "tails_kg": tails_kg, "swu": swu}
    rtol = example["rtol"]
    checks = {}
    ok = True
    for key, expected in example["expected"].items():
        got = computed[key]
        rel = abs(got - expected) / abs(expected)
        passed = rel <= rtol
        ok = ok and passed
        checks[key] = {
            "published": expected,
            "computed": round(got, 6),
            "rel_error": round(rel, 8),
            "rtol": rtol,
            "pass": passed,
        }
    return {
        "id": example["id"],
        "source": example["source"],
        "checks": checks,
        "pass": ok,
    }


def main():
    with open(os.path.join(HERE, "fixtures", "worked_examples.json")) as f:
        fixtures = json.load(f)

    fixture_results = [run_fixture(ex) for ex in fixtures["examples"]]

    # Sample cascade: 4.5 percent product from natural feed, 0.3 percent
    # tails, generic stage factor alpha = 1.4 (a free input, not a machine
    # parameter), 1 kg/s product basis.
    res = solve_ideal_cascade(0.00711, 0.045, 0.003, alpha=1.4, product_flow=1.0)
    inv = check_invariants(res)
    cascade_out = {
        "inputs": {
            "feed_assay": 0.00711,
            "product_assay": 0.045,
            "tails_assay": 0.003,
            "alpha_generic": 1.4,
            "product_flow": 1.0,
        },
        "n_enrich_exact": round(res.n_enrich_exact, 4),
        "n_strip_exact": round(res.n_strip_exact, 4),
        "n_enrich": res.n_enrich,
        "n_strip": res.n_strip,
        "n_total": res.n_total,
        "feed_stage": res.feed_stage,
        "product_assay_achieved": round(res.product_assay_achieved, 8),
        "tails_assay_achieved": round(res.tails_assay_achieved, 8),
        "external_feed_flow": round(res.feed_flow, 8),
        "external_tails_flow": round(res.tails_flow, 8),
        "total_upflow": round(res.total_upflow, 6),
        "upflow_to_feed_ratio": round(res.total_upflow / res.feed_flow, 4),
        "invariants_max_rel_error": {
            k: v for k, v in inv.items() if isinstance(v, float)
        },
        "monotone_assay_profile": inv["monotone"],
        "heads_feed_tails_ordered": inv["ordered"],
        "invariant_tolerance": 1e-9,
        "invariants_pass": all(
            v <= 1e-9 for k, v in inv.items() if isinstance(v, float)
        )
        and inv["monotone"]
        and inv["ordered"],
    }

    tails, feed, swu = tails_tradeoff(0.00711, 0.045, 0.001, 0.005, n=9)
    tradeoff = [
        {"tails_assay": round(float(t), 6), "feed_per_kg_product": round(float(f), 4),
         "swu_per_kg_product": round(float(s), 4)}
        for t, f, s in zip(tails, feed, swu)
    ]

    out = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "worked_examples": fixture_results,
        "worked_examples_pass": all(r["pass"] for r in fixture_results),
        "sample_cascade": cascade_out,
        "tails_tradeoff_demo": tradeoff,
    }

    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    path = os.path.join(ROOT, "results", "physics_validation.json")
    with open(path, "w") as f:
        json.dump(out, f, indent=2)
    print("wrote", path)
    print("worked examples pass:", out["worked_examples_pass"])
    print("cascade invariants pass:", cascade_out["invariants_pass"])


if __name__ == "__main__":
    main()
