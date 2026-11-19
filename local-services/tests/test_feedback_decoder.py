# local-services/tests/test_feedback_decoder.py
"""Tests for the Client Revision Decoder."""
from khensu_services.copilot.feedback_decoder import decode_feedback


def test_warmer_maps_to_low_mid_body_first():
    result = decode_feedback("Could you make the whole thing feel warmer?")
    assert not result["unmatched"]
    match = result["matches"][0]
    assert match["concept"] == "warmer"
    top = match["interpretations"][0]
    assert top["reading"] == "More low-mid body"
    assert top["parameters"]["eq_freq_hz"] == 250.0


def test_multiple_concepts_in_one_note():
    result = decode_feedback(
        "Love it but the vocals are buried and the low end is muddy")
    concepts = {m["concept"] for m in result["matches"]}
    assert "vocal_forward" in concepts
    assert "muddy" in concepts


def test_interpretations_sorted_by_confidence():
    result = decode_feedback("needs more punch")
    interps = result["matches"][0]["interpretations"]
    confs = [i["confidence"] for i in interps]
    assert confs == sorted(confs, reverse=True)


def test_unmatched_flag():
    result = decode_feedback("the moon is made of cheese")
    assert result["unmatched"]
    assert result["matches"] == []


def test_harsh_offers_deess_alternative():
    result = decode_feedback("it's a bit harsh on my earbuds")
    readings = [i["reading"] for i in result["matches"][0]["interpretations"]]
    assert any("de-ess" in r.lower() or "De-ess" in r for r in readings)


def test_endpoint_happy_path(client):
    resp = client.post("/copilot/decode_feedback",
                       json={"feedback_text": "can we make it wider and brighter"})
    assert resp.status_code == 200
    body = resp.json()
    concepts = {m["concept"] for m in body["matches"]}
    assert "wider" in concepts
    assert "brighter" in concepts
    for m in body["matches"]:
        for i in m["interpretations"]:
            assert isinstance(i["parameters"], dict)
