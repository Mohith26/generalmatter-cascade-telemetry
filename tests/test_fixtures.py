import json
import os

import pytest

from cascade import material_balance, separative_work

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "cascade", "fixtures", "worked_examples.json")

with open(FIXTURES) as f:
    EXAMPLES = json.load(f)["examples"]

IDS = [e["id"] for e in EXAMPLES]


@pytest.mark.parametrize("example", EXAMPLES, ids=IDS)
def test_worked_example_cites_a_public_source(example):
    assert example["source"].strip()
    assert "http" in example["source"]


@pytest.mark.parametrize("example", EXAMPLES, ids=IDS)
def test_worked_example_feed_matches_published_value(example):
    inp = example["inputs"]
    feed_kg, tails_kg = material_balance(
        inp["product_kg"], inp["feed_assay"], inp["product_assay"], inp["tails_assay"]
    )
    assert feed_kg == pytest.approx(example["expected"]["feed_kg"], rel=example["rtol"])
    if "tails_kg" in example["expected"]:
        assert tails_kg == pytest.approx(
            example["expected"]["tails_kg"], rel=example["rtol"]
        )


@pytest.mark.parametrize("example", EXAMPLES, ids=IDS)
def test_worked_example_swu_matches_published_value(example):
    inp = example["inputs"]
    swu = separative_work(
        inp["product_kg"], inp["feed_assay"], inp["product_assay"], inp["tails_assay"]
    )
    assert swu == pytest.approx(example["expected"]["swu"], rel=example["rtol"])
