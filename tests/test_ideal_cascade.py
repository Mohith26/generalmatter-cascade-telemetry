import math

import numpy as np
import pytest

from cascade import solve_ideal_cascade, check_invariants

NAT = 0.00711
TOL = 1e-9


def std_case(alpha=1.4):
    return solve_ideal_cascade(NAT, 0.045, 0.003, alpha=alpha, product_flow=1.0)


def test_stage_counts_match_textbook_formulas():
    alpha = 1.4
    res = std_case(alpha)
    beta = math.sqrt(alpha)
    rf = NAT / (1 - NAT)
    rp = 0.045 / (1 - 0.045)
    rw = 0.003 / (1 - 0.003)
    n_e = math.log(rp / rf) / math.log(beta)
    n_s = math.log(rf / rw) / math.log(beta) - 1.0
    assert res.n_enrich == math.ceil(n_e - 1e-12)
    assert res.n_strip == math.ceil(n_s - 1e-12)
    assert res.n_total == res.n_enrich + res.n_strip


def test_achieved_product_assay_meets_or_exceeds_target():
    res = std_case()
    assert res.product_assay_achieved >= res.product_assay_target - 1e-12


def test_achieved_tails_assay_meets_or_undershoots_target():
    res = std_case()
    assert res.tails_assay_achieved <= res.tails_assay_target + 1e-12


def test_stage_total_mass_balance_within_1e9():
    inv = check_invariants(std_case())
    assert inv["stage_total"] <= TOL


def test_stage_isotope_mass_balance_within_1e9():
    inv = check_invariants(std_case())
    assert inv["stage_isotope"] <= TOL


def test_node_total_mass_balance_within_1e9():
    inv = check_invariants(std_case())
    assert inv["node_total"] <= TOL


def test_node_isotope_mass_balance_within_1e9():
    inv = check_invariants(std_case())
    assert inv["node_isotope"] <= TOL


def test_overall_balances_within_1e9():
    inv = check_invariants(std_case())
    assert inv["overall_total"] <= TOL
    assert inv["overall_isotope"] <= TOL


def test_assay_profile_monotone_and_ordered():
    inv = check_invariants(std_case())
    assert inv["monotone"]
    assert inv["ordered"]


def test_all_flows_positive():
    res = std_case()
    assert np.all(res.stage_feed_flow > 0.0)
    assert np.all(res.stage_heads_flow > 0.0)
    assert np.all(res.stage_tails_flow > 0.0)


def test_top_heads_equals_product_flow():
    res = std_case()
    assert res.stage_heads_flow[-1] == pytest.approx(res.product_flow, rel=1e-9)


def test_bottom_tails_equals_external_tails():
    res = std_case()
    assert res.stage_tails_flow[0] == pytest.approx(res.tails_flow, rel=1e-9)


def test_feed_stage_sees_external_feed_assay():
    res = std_case()
    assert res.stage_feed_assay[res.feed_stage - 1] == pytest.approx(NAT, rel=1e-12)


def test_total_upflow_exceeds_external_feed():
    res = std_case()
    assert res.total_upflow > res.feed_flow


def test_larger_alpha_needs_fewer_stages():
    small = std_case(alpha=1.1)
    large = std_case(alpha=2.0)
    assert large.n_total < small.n_total


def test_single_stage_case():
    res = solve_ideal_cascade(0.10, 0.12, 0.08, alpha=3.0, product_flow=1.0)
    assert res.n_enrich == 1
    assert res.n_strip == 0
    inv = check_invariants(res)
    assert inv["stage_isotope"] <= TOL
    assert inv["monotone"] and inv["ordered"]


def test_product_assay_barely_above_feed():
    res = solve_ideal_cascade(NAT, NAT * 1.0001, 0.003, alpha=1.4)
    assert res.n_enrich == 1
    inv = check_invariants(res)
    assert inv["node_isotope"] <= TOL


def test_tails_near_zero_still_solves_cleanly():
    res = solve_ideal_cascade(NAT, 0.045, 1e-5, alpha=1.4)
    assert res.n_strip > res.n_enrich
    inv = check_invariants(res)
    assert inv["stage_isotope"] <= TOL
    assert inv["node_isotope"] <= TOL
    assert inv["monotone"]


def test_invariants_hold_across_alpha_sweep():
    for alpha in (1.05, 1.2, 1.4, 1.7, 2.5):
        inv = check_invariants(std_case(alpha))
        for key in ("stage_total", "stage_isotope", "node_total",
                    "node_isotope", "overall_total", "overall_isotope"):
            assert inv[key] <= TOL, (alpha, key, inv[key])


def test_product_flow_scales_all_flows_linearly():
    a = std_case()
    b = solve_ideal_cascade(NAT, 0.045, 0.003, alpha=1.4, product_flow=5.0)
    assert np.allclose(b.stage_feed_flow, 5.0 * a.stage_feed_flow, rtol=1e-9)
    assert b.feed_flow == pytest.approx(5.0 * a.feed_flow, rel=1e-9)


def test_rejects_alpha_at_or_below_one():
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 0.045, 0.003, alpha=1.0)
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 0.045, 0.003, alpha=0.9)


def test_rejects_bad_assay_ordering():
    with pytest.raises(ValueError):
        solve_ideal_cascade(0.045, NAT, 0.003, alpha=1.4)
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 0.045, 0.008, alpha=1.4)


def test_rejects_out_of_range_assays():
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 1.2, 0.003, alpha=1.4)
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 0.045, 0.0, alpha=1.4)


def test_rejects_non_positive_product_flow():
    with pytest.raises(ValueError):
        solve_ideal_cascade(NAT, 0.045, 0.003, alpha=1.4, product_flow=0.0)
