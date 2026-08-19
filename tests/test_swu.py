import math

import numpy as np
import pytest

from cascade import (
    value_function,
    material_balance,
    separative_work,
    feed_per_product,
    swu_per_product,
    tails_tradeoff,
)

NAT = 0.00711


def test_value_function_zero_at_half():
    assert value_function(0.5) == pytest.approx(0.0, abs=1e-15)


def test_value_function_symmetry():
    for x in (0.001, 0.05, 0.2, 0.45):
        assert value_function(x) == pytest.approx(value_function(1.0 - x), rel=1e-12)


def test_value_function_positive_away_from_half():
    for x in (0.001, 0.1, 0.49, 0.51, 0.9, 0.999):
        assert value_function(x) > 0.0


def test_value_function_matches_direct_formula():
    for x in (0.003, 0.0071, 0.045, 0.3, 0.9):
        direct = (2.0 * x - 1.0) * math.log(x / (1.0 - x))
        assert value_function(x) == pytest.approx(direct, rel=1e-15)


@pytest.mark.parametrize("bad", [0.0, 1.0, -0.1, 1.5])
def test_value_function_domain_errors(bad):
    with pytest.raises(ValueError):
        value_function(bad)


def test_material_balance_conserves_total_and_isotope():
    p = 3.0
    f, w = material_balance(p, NAT, 0.045, 0.003)
    assert f == pytest.approx(p + w, rel=1e-12)
    assert f * NAT == pytest.approx(p * 0.045 + w * 0.003, rel=1e-12)


def test_material_balance_rejects_bad_ordering():
    with pytest.raises(ValueError):
        material_balance(1.0, 0.045, NAT, 0.003)
    with pytest.raises(ValueError):
        material_balance(1.0, NAT, 0.045, 0.045)


def test_material_balance_rejects_negative_product():
    with pytest.raises(ValueError):
        material_balance(-1.0, NAT, 0.045, 0.003)


def test_swu_non_negative_over_grid():
    for xp in (0.01, 0.03, 0.05, 0.2, 0.9):
        for xw in (0.0005, 0.002, 0.004, 0.006):
            assert separative_work(1.0, NAT, xp, xw) >= 0.0


def test_swu_vanishes_when_product_approaches_feed():
    tiny = separative_work(1.0, NAT, NAT * 1.000001, NAT * 0.999999)
    assert 0.0 <= tiny < 1e-4


def test_swu_scales_linearly_with_product_mass():
    one = separative_work(1.0, NAT, 0.045, 0.003)
    ten = separative_work(10.0, NAT, 0.045, 0.003)
    assert ten == pytest.approx(10.0 * one, rel=1e-12)


def test_feed_per_product_rises_with_higher_tails_assay():
    lo = feed_per_product(NAT, 0.045, 0.002)
    hi = feed_per_product(NAT, 0.045, 0.004)
    assert hi > lo


def test_swu_per_product_falls_with_higher_tails_assay():
    lo = swu_per_product(NAT, 0.045, 0.002)
    hi = swu_per_product(NAT, 0.045, 0.004)
    assert hi < lo


def test_swu_per_product_rises_with_product_assay():
    grid = [0.01, 0.02, 0.05, 0.1, 0.2]
    vals = [swu_per_product(NAT, xp, 0.003) for xp in grid]
    assert all(b > a for a, b in zip(vals, vals[1:]))


def test_tails_tradeoff_monotone_directions():
    tails, feed, swu = tails_tradeoff(NAT, 0.045, 0.001, 0.005, n=25)
    assert np.all(np.diff(feed) > 0.0)
    assert np.all(np.diff(swu) < 0.0)
    assert len(tails) == 25


def test_tails_tradeoff_rejects_bad_range():
    with pytest.raises(ValueError):
        tails_tradeoff(NAT, 0.045, 0.005, 0.001)
    with pytest.raises(ValueError):
        tails_tradeoff(NAT, 0.045, 0.001, 0.008)
