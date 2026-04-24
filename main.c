#include "tcache.h"

#include <stdio.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64ULL
#define SAME_L1_DATA_SET_STRIDE (256ULL * CACHE_LINE_SIZE)
#define SAME_L2_SET_STRIDE (8192ULL * CACHE_LINE_SIZE)

static void expect(int condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "TEST FAILED: %s\n", message);
		exit(EXIT_FAILURE);
	}
}

static void seed_line(uint64_t base_addr, uint8_t seed) {
	uint64_t aligned_base = base_addr & ~(CACHE_LINE_SIZE - 1ULL);
	uint64_t i;

	for (i = 0; i < CACHE_LINE_SIZE; i++) {
		write_memory(aligned_base + i, (uint8_t)(seed + i));
	}
}

static void test_basic_data_hits(void) {
	uint64_t addr = 0x1000ULL + 7ULL;
	cache_stats_t l1_stats;
	cache_stats_t l2_stats;

	seed_line(addr, 0x20U);
	init_cache(LRU);

	expect(read_cache(addr, DATA) == 0x27U, "first DATA read should come from memory");
	l1_stats = get_l1_data_stats();
	l2_stats = get_l2_stats();
	expect(l1_stats.accesses == 1 && l1_stats.misses == 1, "first DATA read should miss in L1");
	expect(l2_stats.accesses == 1 && l2_stats.misses == 1, "first DATA read should miss in L2");
	expect(get_l1_data_cache_line(addr) != NULL, "line should be installed in L1-D after miss");

	expect(read_cache(addr, DATA) == 0x27U, "second DATA read should hit in L1");
	l1_stats = get_l1_data_stats();
	l2_stats = get_l2_stats();
	expect(l1_stats.accesses == 2 && l1_stats.misses == 1, "second DATA read should hit in L1");
	expect(l2_stats.accesses == 1 && l2_stats.misses == 1, "second DATA read should not touch L2");
}

static void test_l1_coherency(void) {
	uint64_t addr = 0x4000ULL + 3ULL;
	cache_stats_t instr_stats;
	cache_stats_t data_stats;
	cache_stats_t l2_stats;

	seed_line(addr, 0x40U);
	init_cache(LRU);

	expect(read_cache(addr, INSTR) == 0x43U, "instruction read should fetch the initial value");
	expect(read_cache(addr, DATA) == 0x43U, "data read should see the same initial value");
	expect(get_l1_instr_cache_line(addr) != NULL, "line should exist in L1-I before the write");
	expect(get_l1_data_cache_line(addr) != NULL, "line should exist in L1-D before the write");

	write_cache(addr, 0xABU, DATA);
	expect(get_l1_instr_cache_line(addr) == NULL, "writing through L1-D should invalidate L1-I");

	expect(read_cache(addr, INSTR) == 0xABU, "instruction read should observe the updated value");
	expect(get_l1_data_cache_line(addr) != NULL, "line should remain in L1-D after the writeback");
	expect(get_l1_data_cache_line(addr)->modified == 0, "forced writeback should clean the L1-D copy");

	instr_stats = get_l1_instr_stats();
	data_stats = get_l1_data_stats();
	l2_stats = get_l2_stats();
	expect(instr_stats.accesses == 2 && instr_stats.misses == 2, "both instruction-side accesses should miss");
	expect(data_stats.accesses == 2 && data_stats.misses == 1, "only the first data-side access should miss");
	expect(l2_stats.accesses == 4 && l2_stats.misses == 1, "coherency writeback should count as an extra L2 access");
}

static void test_inclusive_dirty_l2_eviction(void) {
	uint64_t victim_addr = 0x8000ULL + 5ULL;
	uint64_t conflict_addrs[4];
	int i;

	seed_line(victim_addr, 0x10U);
	for (i = 0; i < 4; i++) {
		conflict_addrs[i] = victim_addr + ((uint64_t)(i + 1) * SAME_L2_SET_STRIDE);
		seed_line(conflict_addrs[i], (uint8_t)(0x30U + (uint8_t)(i * 0x10U)));
	}

	init_cache(LRU);

	expect(read_cache(victim_addr, INSTR) == 0x15U, "victim line should be loaded into L1-I");
	write_cache(victim_addr, 0xEEU, INSTR);

	for (i = 0; i < 4; i++) {
		(void)read_cache(conflict_addrs[i], DATA);
	}

	expect(get_l2_cache_line(victim_addr) == NULL, "victim line should be evicted from L2");
	expect(get_l1_instr_cache_line(victim_addr) == NULL, "L2 eviction should invalidate the inclusive L1-I copy");
	expect(read_memory(victim_addr) == 0xEEU, "dirty L1-I data should reach memory on L2 eviction");
}

static uint64_t run_policy_workload(replacement_policy_e policy) {
	uint64_t base_addr = 0x12000ULL;
	uint64_t addrs[3];
	cache_stats_t stats;
	int i;
	int iter;

	for (i = 0; i < 3; i++) {
		addrs[i] = base_addr + ((uint64_t)i * SAME_L1_DATA_SET_STRIDE);
		seed_line(addrs[i], (uint8_t)(0x60U + (uint8_t)(i * 0x10U)));
	}

	init_cache(policy);
	for (iter = 0; iter < 2000; iter++) {
		for (i = 0; i < 3; i++) {
			(void)read_cache(addrs[i], DATA);
		}
	}

	stats = get_l1_data_stats();
	return stats.misses;
}

int main(int argc, char *argv[]) {
	uint64_t lru_misses;
	uint64_t random_misses;

	(void)argc;
	(void)argv;

	test_basic_data_hits();
	test_l1_coherency();
	test_inclusive_dirty_l2_eviction();

	lru_misses = run_policy_workload(LRU);
	random_misses = run_policy_workload(RANDOM);

	printf("Unit tests passed.\n");
	printf("Replacement policy comparison workload:\n");
	printf("  LRU L1-D misses: %llu\n", (unsigned long long)lru_misses);
	printf("  RANDOM L1-D misses: %llu\n", (unsigned long long)random_misses);

	return 0;
}
