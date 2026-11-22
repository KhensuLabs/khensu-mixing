"""MAC-1: the memory report reads the process size and the RAM on every
desktop platform (macOS has neither /proc nor SC_PHYS_PAGES)."""
import sys

import pytest

from khensu_services.common.memory_gate import physical_memory_bytes
from khensu_services.common.router_memory import process_rss_bytes


@pytest.mark.skipif(sys.platform not in ("win32", "darwin", "linux"), reason="desktop platforms")
def test_rss_and_ram_are_measured():
    rss, ram = process_rss_bytes(), physical_memory_bytes()
    assert rss is not None and 10 * 2**20 < rss < 64 * 2**30
    assert ram is not None and ram >= 2 * 2**30
    assert rss < ram
