# HW11 Cache Simulator

Name: `<Your Name>`

EID: `<Your EID>`

## Build And Run

Recommended starter-code build:

```bash
bash build.sh
./build/hw11
```

Local fallback if `cmake` is unavailable:

```bash
cc -std=c99 main.c libtcache/tcache.c libtcache/tcache_backend.c -Ilibtcache -o hw11_local
./hw11_local
```

## Testing Methodology

The tests in `main.c` cover both correctness and replacement-policy comparison. I added unit tests for three behaviors that are easy to get wrong in this assignment: basic L1/L2 hit and miss accounting, L1 instruction/data coherency when one side writes a line and the other side later reads it, and inclusive L2 eviction when an L1 copy is still present and dirty. These tests check returned values, cache presence through the provided `get_*_cache_line` helpers, modified-bit transitions, and the access/miss counters defined in the spec clarifications.

For the replacement-policy comparison, I used a repeatable workload that accesses three addresses mapping to the same 2-way L1 data-cache set. That pattern intentionally overfills the set so the replacement decision matters, while the addresses map to different L2 sets so the comparison is focused on the L1 set-associative behavior rather than L2 conflict misses. I ran the workload once with `LRU` and once with `RANDOM` after reinitializing the cache each time.

## Local Results

On the current local test workload, the unit tests passed and the policy-comparison run produced:

- `LRU` L1-D misses: `6000`
- `RANDOM` L1-D misses: `3001`

These numbers come from the deterministic local random generator used in `tcache.c`, so repeated runs on the same code produce the same comparison output.
