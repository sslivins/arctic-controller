from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside


FLASH_SIZE = 16 * 1024 * 1024


def _parse_size(value: str) -> int:
    value = value.strip()
    suffixes = {"K": 1024, "M": 1024 * 1024}
    if value[-1:].upper() in suffixes:
        return int(value[:-1], 0) * suffixes[value[-1].upper()]
    return int(value, 0)


def test_partition_layout_is_non_overlapping_and_leaves_reserve():
    path = Path(__file__).parents[1] / "partitions.csv"
    partitions = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        name, part_type, subtype, offset, size, *_ = [
            field.strip() for field in line.split(",")
        ]
        partitions.append(
            {
                "name": name,
                "type": part_type,
                "subtype": subtype,
                "offset": _parse_size(offset),
                "size": _parse_size(size),
            }
        )

    for previous, current in zip(partitions, partitions[1:]):
        assert previous["offset"] + previous["size"] <= current["offset"]

    by_name = {partition["name"]: partition for partition in partitions}
    assert by_name["nvs"]["size"] == 256 * 1024
    assert by_name["ota_0"]["size"] == 4 * 1024 * 1024
    assert by_name["ota_1"]["size"] == 4 * 1024 * 1024
    assert by_name["history"]["offset"] == 0x910000
    assert by_name["history"]["size"] == 2 * 1024 * 1024

    # Everything must fit within the flash device.
    used_end = max(p["offset"] + p["size"] for p in partitions)
    assert used_end <= FLASH_SIZE

    # Headroom is now an explicit, named "reserved" partition (named scratch space
    # so future features don't require another partition-table change / re-flash)
    # plus any still-unallocated tail — rather than unallocated space alone.
    reserve = by_name.get("reserved", {}).get("size", 0) + (FLASH_SIZE - used_end)
    assert reserve >= 3 * 1024 * 1024
