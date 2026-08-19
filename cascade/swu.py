"""Separative work accounting for a two component isotope mixture.

Everything in this module is standard open literature material: the Dirac
value function, separative work units (SWU), and external stream material
balances, as presented in Benedict, Pigford and Levi, "Nuclear Chemical
Engineering", 2nd ed., McGraw-Hill, 1981 (chapter 12) and reproduced in many
public references (for example the IAEA Bulletin and the Wikipedia article on
separative work units).

Assays are mass fractions expressed as plain fractions, so 0.00711 means
0.711 percent. No machine or plant specific data appears anywhere here.
"""

import math

import numpy as np


def value_function(x):
    """Dirac value function V(x) = (2x - 1) ln(x / (1 - x)).

    Defined on the open interval (0, 1). V(0.5) = 0 and V(x) = V(1 - x).
    """
    if not 0.0 < x < 1.0:
        raise ValueError("assay must be strictly inside (0, 1), got %r" % (x,))
    return (2.0 * x - 1.0) * math.log(x / (1.0 - x))


def _check_assays(feed_assay, product_assay, tails_assay):
    for name, v in (
        ("feed", feed_assay),
        ("product", product_assay),
        ("tails", tails_assay),
    ):
        if not 0.0 < v < 1.0:
            raise ValueError("%s assay must be in (0, 1), got %r" % (name, v))
    if not tails_assay < feed_assay < product_assay:
        raise ValueError(
            "assays must satisfy tails < feed < product, got "
            "tails=%r feed=%r product=%r"
            % (tails_assay, feed_assay, product_assay)
        )


def material_balance(product_kg, feed_assay, product_assay, tails_assay):
    """External stream balance. Returns (feed_kg, tails_kg) for product_kg.

    F = P (xp - xw) / (xf - xw) and W = F - P follow from conserving total
    mass and the tracked isotope across the three external streams.
    """
    _check_assays(feed_assay, product_assay, tails_assay)
    if product_kg < 0.0:
        raise ValueError("product mass must be non negative")
    feed_kg = product_kg * (product_assay - tails_assay) / (feed_assay - tails_assay)
    tails_kg = feed_kg - product_kg
    return feed_kg, tails_kg


def separative_work(product_kg, feed_assay, product_assay, tails_assay):
    """SWU for producing product_kg of product: P V(xp) + W V(xw) - F V(xf)."""
    feed_kg, tails_kg = material_balance(
        product_kg, feed_assay, product_assay, tails_assay
    )
    return (
        product_kg * value_function(product_assay)
        + tails_kg * value_function(tails_assay)
        - feed_kg * value_function(feed_assay)
    )


def feed_per_product(feed_assay, product_assay, tails_assay):
    """Feed mass required per unit product mass."""
    return material_balance(1.0, feed_assay, product_assay, tails_assay)[0]


def swu_per_product(feed_assay, product_assay, tails_assay):
    """Separative work per unit product mass."""
    return separative_work(1.0, feed_assay, product_assay, tails_assay)


def tails_tradeoff(feed_assay, product_assay, tails_lo, tails_hi, n=50):
    """SWU per product and feed per product across a range of tails assays.

    This is the classic optimal tails intuition demo: lowering the tails assay
    stretches more product out of each kilogram of feed but costs more
    separative work, so feed per product falls while SWU per product rises.
    Returns (tails_assays, feed_per_product, swu_per_product) as numpy arrays.
    """
    if not 0.0 < tails_lo < tails_hi < feed_assay:
        raise ValueError("need 0 < tails_lo < tails_hi < feed assay")
    tails = np.linspace(tails_lo, tails_hi, n)
    feed = np.array([feed_per_product(feed_assay, product_assay, w) for w in tails])
    swu = np.array([swu_per_product(feed_assay, product_assay, w) for w in tails])
    return tails, feed, swu
