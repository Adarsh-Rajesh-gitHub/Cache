# HW11 Cache Simulator

Name: Adarsh Rajesh

EID: ar77947

## Build And Run

Recommended starter-code build:

```bash
bash build.sh
```

Then run one of these modes:

```bash
./build/hw11 unit
./build/hw11 compare
./build/hw11 all
```

Mode summary:

- `unit` runs only the unit-test suite.
- `compare` runs only the replacement-policy comparison report.
- `all` runs both the unit tests and the comparison report. This is also the default if you run `./build/hw11` with no extra argument.

Local fallback if `cmake` is unavailable:

```bash
cc -std=c99 main.c libtcache/tcache.c libtcache/tcache_backend.c -Ilibtcache -o hw11_local
./hw11_local unit
./hw11_local compare
./hw11_local all
```

## Unit Tests

The unit tests in `main.c` are meant to validate both correctness and a few of the grading-sensitive corner cases. The current suite checks:

- basic L1-D read miss then hit
- basic L1-I read miss then hit
- write hit updates the cached byte and marks the line dirty
- write miss uses write-allocate
- dirty L1-D eviction writes back to L2 instead of main memory
- L1 instruction/data coherency for the same address
- dirty inclusive L2 eviction from an L1-I-resident line
- dirty back-invalidation of an L1-D line during L2 eviction
- 2-way L1-D set associativity behavior
- `init_cache()` fully resets cache contents and statistics

## Testing Methodology

I used two kinds of testing. First, I added unit tests to check cache correctness at the line level: hits and misses, write-back behavior, write-allocate behavior, inclusion, back-invalidation, and I/D coherency. These tests use predictable seeded memory values, then validate returned bytes, cache residency through the provided `get_*_cache_line` helpers, modified bits, and the L1/L2 access and miss counters described in the clarification post.

Second, I added two separate replacement-policy comparison workloads for the 2-way L1 data cache. The first is an adversarial cyclic conflict pattern using three addresses `A`, `B`, and `C` that all map to the same L1-D set, accessed as `A, B, C, A, B, C, ...`. This is a pathological case for perfect LRU in a 2-way cache, so it is useful for showing that `RANDOM` can sometimes outperform `LRU`. The second workload is a more locality-friendly hot-set pattern where `A` and `B` are the true working set and `C` is only an occasional intruder. The loop accesses `A, B, A, B, A, C, ...`, which lets LRU usually preserve the hot line that will be reused first after the intrusion. This gives a more intuitive case where `LRU` should outperform `RANDOM`.

Taken together, these two workloads make the comparison stronger than using only one benchmark. The results show that replacement-policy quality depends on the access pattern: `LRU` is usually better when locality is stable, but `RANDOM` can do surprisingly well on cyclic conflict-thrashing patterns that defeat recency-based replacement.

## Local Comparison Results

On my local comparison runs, I observed the following:

- Conflict thrash workload:
  `LRU` L1-D accesses=`6000`, misses=`6000`; `RANDOM` L1-D accesses=`6000`, misses=`3001`
- Hot-set-with-intruder workload:
  `LRU` L1-D accesses=`30002`, misses=`10001`; `RANDOM` L1-D accesses=`30002`, misses=`15000`

These results match the intended interpretation of the workloads:

- On the cyclic thrash pattern, `RANDOM` beats `LRU`.
- On the more normal hot-set pattern, `LRU` beats `RANDOM`.

The local test harness also prints the corresponding L2 access and miss counts so the comparison report includes more than just a single miss number.

## How To Verify My Work

If you want to check only the unit tests, run:

```bash
./build/hw11 unit
```

If you want to check only the replacement-policy comparison report, run:

```bash
./build/hw11 compare
```

If you want to rerun everything exactly the way I verified it locally, run:

```bash
bash build.sh
./build/hw11 all
```

If `cmake` is not available on your machine, use:

```bash
cc -std=c99 main.c libtcache/tcache.c libtcache/tcache_backend.c -Ilibtcache -o hw11_local
./hw11_local all
```

When the comparison report is working correctly, it should print both workloads and show:

- `RANDOM` with fewer L1-D misses than `LRU` on the conflict-thrash workload
- `LRU` with fewer L1-D misses than `RANDOM` on the hot-set-with-intruder workload
