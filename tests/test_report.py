import json
import zipfile

from host.report import redact, redact_text, write_report


def test_redaction_covers_nested_secrets():
    assert redact({"nested": {"auth_secret": "abc", "safe": 1}})["nested"] == {
        "auth_secret": "<redacted>",
        "safe": 1,
    }
    assert "ab" * 32 not in redact_text("Bearer " + "ab" * 32)


def test_support_bundle_contains_report_without_secret(tmp_path):
    output = tmp_path / "bundle.zip"
    write_report(output, {"auth_secret": "secret", "result": "PASS"}, tmp_path)
    with zipfile.ZipFile(output) as bundle:
        report = json.loads(bundle.read("commissioning.json"))
    assert report["results"]["auth_secret"] == "<redacted>"
