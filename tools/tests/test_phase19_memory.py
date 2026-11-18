from __future__ import annotations

import random
import unittest

PAGE = 4096
DMA32_PFN_LIMIT = (1 << 32) // PAGE


def merge(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    result: list[list[int]] = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1][1] = max(result[-1][1], end)
        else:
            result.append([start, end])
    return [(start, end) for start, end in result]


def normalize(usable_bytes: list[tuple[int, int]],
              reserved_bytes: list[tuple[int, int]]) -> list[tuple[int, int]]:
    usable: list[tuple[int, int]] = []
    reserved: list[tuple[int, int]] = []
    for base, length in usable_bytes:
        if length <= 0 or base < 0 or base + length >= 1 << 64:
            raise ValueError("invalid usable range")
        start = (base + PAGE - 1) // PAGE
        end = (base + length) // PAGE
        if start == 0:
            start = 1
        if end > start:
            usable.append((start, end))
    for base, length in reserved_bytes:
        if length <= 0 or base < 0 or base + length >= 1 << 64:
            raise ValueError("invalid reserved range")
        start = base // PAGE
        end = (base + length + PAGE - 1) // PAGE
        if end > start:
            reserved.append((start, end))
    usable = merge(usable)
    reserved = merge(reserved)

    available: list[tuple[int, int]] = []
    for start, end in usable:
        cursor = start
        for rstart, rend in reserved:
            if rend <= cursor:
                continue
            if rstart >= end:
                break
            if rstart > cursor:
                available.append((cursor, min(rstart, end)))
            cursor = max(cursor, rend)
            if cursor >= end:
                break
        if cursor < end:
            available.append((cursor, end))
    return merge(available)


class BuddyModel:
    """Small host-side model of the kernel's per-region Buddy policy."""

    def __init__(self, regions: list[tuple[int, int]]) -> None:
        self.free: dict[tuple[int, int, int], set[int]] = {}
        self.allocated: dict[tuple[int, int], int] = {}
        self.initial_pages = sum(end - start for start, end in regions)
        for region_id, (start, end) in enumerate(regions):
            cursor = start
            while cursor < end:
                order = min(40, (end - cursor).bit_length() - 1)
                while cursor & ((1 << order) - 1):
                    order -= 1
                self._list(region_id, order).add(cursor)
                cursor += 1 << order

    def _list(self, region: int, order: int) -> set[int]:
        return self.free.setdefault((region, order, 0), set())

    def allocate(self, order: int, zone: int | None = None) -> tuple[int, int] | None:
        for region in sorted({key[0] for key in self.free}):
            for found in range(order, 41):
                free = self._list(region, found)
                candidates = sorted(
                    pfn for pfn in free
                    if zone is None or (pfn < DMA32_PFN_LIMIT) == (zone == 0)
                )
                if not candidates:
                    continue
                pfn = candidates[0]
                free.remove(pfn)
                while found > order:
                    found -= 1
                    self._list(region, found).add(pfn + (1 << found))
                self.allocated[(region, pfn)] = order
                return region, pfn
        return None

    def release(self, region: int, pfn: int, order: int) -> bool:
        key = (region, pfn)
        if self.allocated.get(key) != order:
            return False
        del self.allocated[key]
        current = order
        while current < 40:
            buddy = pfn ^ (1 << current)
            buddies = self._list(region, current)
            if buddy not in buddies:
                break
            buddies.remove(buddy)
            pfn = min(pfn, buddy)
            current += 1
        self._list(region, current).add(pfn)
        return True

    def free_pages(self) -> int:
        return sum(1 << order for (_, order, _), blocks in self.free.items()
                   for _pfn in blocks)

    def assert_invariants(self, testcase: unittest.TestCase) -> None:
        occupied: list[tuple[int, int, str]] = []
        for (region, order, _), blocks in self.free.items():
            for pfn in blocks:
                occupied.append((pfn, pfn + (1 << order), f"free:{region}"))
        for (region, pfn), order in self.allocated.items():
            occupied.append((pfn, pfn + (1 << order), f"alloc:{region}"))
        occupied.sort()
        for previous, current in zip(occupied, occupied[1:]):
            testcase.assertLessEqual(
                previous[1], current[0],
                f"overlap: {previous!r} vs {current!r}",
            )
        testcase.assertEqual(
            sum(end - start for start, end, _ in occupied),
            self.initial_pages,
        )
        testcase.assertEqual(
            self.free_pages() +
            sum(1 << order for order in self.allocated.values()),
            self.initial_pages,
        )


class Phase19MemoryTests(unittest.TestCase):
    def test_reserved_ranges_win_and_alignment_is_conservative(self) -> None:
        usable = [(0, 64 * PAGE), (64 * PAGE, 64 * PAGE)]
        reserved = [(7 * PAGE + 1, 2 * PAGE), (20 * PAGE, 3 * PAGE)]
        self.assertEqual(
            normalize(usable, reserved),
            [(1, 7), (10, 20), (23, 128)],
        )

    def test_zero_and_fully_reserved_regions_produce_no_free_range(self) -> None:
        self.assertEqual(normalize([(0, PAGE)], []), [])
        self.assertEqual(
            normalize([(0, 8 * PAGE)], [(0, 8 * PAGE)]),
            [],
        )

    def test_split_all_orders_then_coalesce_to_original_extent(self) -> None:
        allocator = BuddyModel([(1, 1025)])
        before = allocator.free_pages()
        live: list[tuple[int, int, int]] = []
        for order in range(0, 9):
            allocated = allocator.allocate(order)
            self.assertIsNotNone(allocated)
            region, pfn = allocated
            live.append((region, pfn, order))
            allocator.assert_invariants(self)
        for region, pfn, order in reversed(live):
            self.assertTrue(allocator.release(region, pfn, order))
            allocator.assert_invariants(self)
        self.assertEqual(allocator.free_pages(), before)

    def test_wrong_order_and_double_free_are_rejected(self) -> None:
        allocator = BuddyModel([(64, 256)])
        region, pfn = allocator.allocate(2)  # type: ignore[misc]
        self.assertFalse(allocator.release(region, pfn, 1))
        self.assertTrue(allocator.release(region, pfn, 2))
        self.assertFalse(allocator.release(region, pfn, 2))
        allocator.assert_invariants(self)

    def test_random_allocate_free_keeps_ranges_disjoint(self) -> None:
        rng = random.Random(0x19BADD1E)
        allocator = BuddyModel([(1, 8193), (16384, 24576)])
        live: list[tuple[int, int, int]] = []
        for _ in range(12_000):
            if live and rng.random() < 0.48:
                index = rng.randrange(len(live))
                region, pfn, order = live.pop(index)
                self.assertTrue(allocator.release(region, pfn, order))
            else:
                allocated = allocator.allocate(rng.randrange(0, 7))
                if allocated is not None:
                    region, pfn = allocated
                    actual_order = allocator.allocated[(region, pfn)]
                    live.append((region, pfn, actual_order))
            allocator.assert_invariants(self)
        for region, pfn, order in live:
            self.assertTrue(allocator.release(region, pfn, order))
        self.assertEqual(allocator.free_pages(), allocator.initial_pages)
        allocator.assert_invariants(self)


if __name__ == "__main__":
    unittest.main()
