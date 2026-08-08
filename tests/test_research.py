from pathlib import Path
from unittest.mock import patch

from host.research import android_info


def test_android_info_accepts_space_separated_adb_output():
    output = "List of devices attached\nserial  device product:test model:Phone\n"
    with patch("host.research._run", side_effect=[output, "adb", "Phone", "16", "fingerprint"]):
        assert android_info(Path("adb"))["model"] == "Phone"
