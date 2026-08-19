"""Ideal (matched abundance ratio) cascade solver.

Implements the textbook ideal cascade from open literature: Benedict,
Pigford and Levi, "Nuclear Chemical Engineering", 2nd ed., McGraw-Hill,
1981, chapter 12. The stage separation factor alpha is a generic free input
parameter. Nothing here encodes the separative performance, geometry, or
operating parameters of any real machine.

Model summary. Each stage splits its feed into heads (up flow) and tails
(down flow). With overall stage factor alpha and the ideal cascade condition,
the heads separation factor beta satisfies beta^2 = alpha, and abundance
ratios step by a factor beta per stage. Heads of stage s feed stage s+1 and
tails of stage s+1 return to stage s, so streams that mix always have equal
assay (the no mixing condition). Stage counts follow from

    n_enrich = ln(Rp / Rf) / ln(beta)
    n_strip  = ln(Rf / Rw) / ln(beta) - 1

where R = x / (1 - x). Integer stage counts are the ceilings, so the achieved
product assay meets or slightly exceeds the target and the achieved tails
assay meets or is slightly below the target. Interstage flows are found by
solving the exact stage by stage material balance as a tridiagonal linear
system (Thomas algorithm), which lets the tests verify total and isotope
conservation at every stage to tight tolerance instead of assuming it.
"""

import math
from dataclasses import dataclass, field
from typing import List

import numpy as np


def abundance_ratio(x):
    return x / (1.0 - x)


def assay_from_ratio(r):
    return r / (1.0 + r)


@dataclass
class CascadeResult:
    feed_assay: float
    product_assay_target: float
    tails_assay_target: float
    alpha: float
    beta: float
    n_enrich_exact: float
    n_strip_exact: float
    n_enrich: int
    n_strip: int
    n_total: int
    feed_stage: int
    product_assay_achieved: float
    tails_assay_achieved: float
    product_flow: float
    feed_flow: float
    tails_flow: float
    total_upflow: float
    stage_feed_flow: np.ndarray = field(repr=False)
    stage_heads_flow: np.ndarray = field(repr=False)
    stage_tails_flow: np.ndarray = field(repr=False)
    stage_feed_assay: np.ndarray = field(repr=False)
    stage_heads_assay: np.ndarray = field(repr=False)
    stage_tails_assay: np.ndarray = field(repr=False)


def _thomas_solve(sub, diag, sup, rhs):
    """Solve a tridiagonal system in O(n). Arrays are modified copies."""
    n = len(diag)
    c = np.array(sup, dtype=float)
    d = np.array(diag, dtype=float)
    a = np.array(sub, dtype=float)
    b = np.array(rhs, dtype=float)
    for i in range(1, n):
        m = a[i] / d[i - 1]
        d[i] -= m * c[i - 1]
        b[i] -= m * b[i - 1]
    x = np.zeros(n)
    x[n - 1] = b[n - 1] / d[n - 1]
    for i in range(n - 2, -1, -1):
        x[i] = (b[i] - c[i] * x[i + 1]) / d[i]
    return x


def solve_ideal_cascade(feed_assay, product_assay, tails_assay, alpha, product_flow=1.0):
    """Solve an ideal cascade for the given assays and stage factor.

    Stages are numbered 1 (bottom stripping stage) through n_total (top
    enriching stage). The feed stage is n_strip + 1 and is counted in the
    enriching section. Product is the heads of the top stage, plant tails is
    the tails of stage 1.
    """
    for name, v in (
        ("feed", feed_assay),
        ("product", product_assay),
        ("tails", tails_assay),
    ):
        if not 0.0 < v < 1.0:
            raise ValueError("%s assay must be in (0, 1), got %r" % (name, v))
    if not tails_assay < feed_assay < product_assay:
        raise ValueError("assays must satisfy tails < feed < product")
    if alpha <= 1.0:
        raise ValueError("stage separation factor alpha must exceed 1")
    if product_flow <= 0.0:
        raise ValueError("product flow must be positive")

    beta = math.sqrt(alpha)
    log_beta = math.log(beta)
    rf = abundance_ratio(feed_assay)
    rp = abundance_ratio(product_assay)
    rw = abundance_ratio(tails_assay)

    n_enrich_exact = math.log(rp / rf) / log_beta
    n_strip_exact = math.log(rf / rw) / log_beta - 1.0
    n_enrich = max(1, math.ceil(n_enrich_exact - 1e-12))
    n_strip = max(0, math.ceil(n_strip_exact - 1e-12))
    n_total = n_enrich + n_strip
    feed_stage = n_strip + 1

    # Stage feed abundance ratios, anchored so the feed stage sees exactly
    # the external feed assay. beta > 1 makes the profile strictly monotone.
    stage_idx = np.arange(1, n_total + 1)
    r_feed = rf * beta ** (stage_idx - feed_stage)
    x_feed = assay_from_ratio(r_feed)
    x_heads = assay_from_ratio(beta * r_feed)
    x_tails = assay_from_ratio(r_feed / beta)

    # Stage cut from the per stage isotope balance.
    theta = (x_feed - x_tails) / (x_heads - x_tails)

    product_assay_achieved = assay_from_ratio(rf * beta ** n_enrich)
    tails_assay_achieved = assay_from_ratio(rf * beta ** (-(n_strip + 1)))

    feed_flow = (
        product_flow
        * (product_assay_achieved - tails_assay_achieved)
        / (feed_assay - tails_assay_achieved)
    )
    tails_flow = feed_flow - product_flow

    # Node balances: F_s = theta_{s-1} F_{s-1} + (1 - theta_{s+1}) F_{s+1}
    # plus the external feed at the feed stage. Tridiagonal in stage feeds.
    n = n_total
    sub = np.zeros(n)
    diag = np.ones(n)
    sup = np.zeros(n)
    rhs = np.zeros(n)
    for i in range(n):
        if i > 0:
            sub[i] = -theta[i - 1]
        if i < n - 1:
            sup[i] = -(1.0 - theta[i + 1])
    rhs[feed_stage - 1] = feed_flow
    stage_feed_flow = _thomas_solve(sub, diag, sup, rhs)
    stage_heads_flow = theta * stage_feed_flow
    stage_tails_flow = (1.0 - theta) * stage_feed_flow

    top_product = stage_heads_flow[-1]
    if not math.isclose(top_product, product_flow, rel_tol=1e-8):
        raise RuntimeError(
            "cascade solve failed closure: top heads %r vs product %r"
            % (top_product, product_flow)
        )

    return CascadeResult(
        feed_assay=feed_assay,
        product_assay_target=product_assay,
        tails_assay_target=tails_assay,
        alpha=alpha,
        beta=beta,
        n_enrich_exact=n_enrich_exact,
        n_strip_exact=n_strip_exact,
        n_enrich=n_enrich,
        n_strip=n_strip,
        n_total=n_total,
        feed_stage=feed_stage,
        product_assay_achieved=product_assay_achieved,
        tails_assay_achieved=tails_assay_achieved,
        product_flow=product_flow,
        feed_flow=feed_flow,
        tails_flow=tails_flow,
        total_upflow=float(np.sum(stage_heads_flow)),
        stage_feed_flow=stage_feed_flow,
        stage_heads_flow=stage_heads_flow,
        stage_tails_flow=stage_tails_flow,
        stage_feed_assay=x_feed,
        stage_heads_assay=x_heads,
        stage_tails_assay=x_tails,
    )


def check_invariants(res):
    """Return max relative violations of the cascade conservation laws.

    Keys:
      stage_total: per stage total mass closure F = H + T
      stage_isotope: per stage isotope closure F x = H y + T z
      node_total: interstage node total mass closure
      node_isotope: interstage node isotope closure
      overall_total: external total mass closure
      overall_isotope: external isotope closure
      monotone: True when stage feed assays strictly increase bottom to top
      ordered: True when heads > feed > tails assay at every stage
    """
    n = res.n_total
    F = res.stage_feed_flow
    H = res.stage_heads_flow
    T = res.stage_tails_flow
    x = res.stage_feed_assay
    y = res.stage_heads_assay
    z = res.stage_tails_assay

    stage_total = np.max(np.abs(F - H - T) / F)
    stage_isotope = np.max(np.abs(F * x - H * y - T * z) / (F * x))

    node_total = 0.0
    node_isotope = 0.0
    for s in range(1, n + 1):
        inflow = 0.0
        iso_in = 0.0
        if s > 1:
            inflow += H[s - 2]
            iso_in += H[s - 2] * y[s - 2]
        if s < n:
            inflow += T[s]
            iso_in += T[s] * z[s]
        if s == res.feed_stage:
            inflow += res.feed_flow
            iso_in += res.feed_flow * res.feed_assay
        node_total = max(node_total, abs(inflow - F[s - 1]) / F[s - 1])
        node_isotope = max(node_isotope, abs(iso_in - F[s - 1] * x[s - 1]) / (F[s - 1] * x[s - 1]))

    overall_total = abs(res.feed_flow - res.product_flow - res.tails_flow) / res.feed_flow
    iso_feed = res.feed_flow * res.feed_assay
    iso_out = (
        res.product_flow * res.product_assay_achieved
        + res.tails_flow * res.tails_assay_achieved
    )
    overall_isotope = abs(iso_feed - iso_out) / iso_feed

    monotone = bool(np.all(np.diff(x) > 0.0))
    ordered = bool(np.all(y > x) and np.all(x > z))

    return {
        "stage_total": float(stage_total),
        "stage_isotope": float(stage_isotope),
        "node_total": float(node_total),
        "node_isotope": float(node_isotope),
        "overall_total": float(overall_total),
        "overall_isotope": float(overall_isotope),
        "monotone": monotone,
        "ordered": ordered,
    }
