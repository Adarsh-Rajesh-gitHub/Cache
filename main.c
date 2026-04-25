#include "tcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_LINE_SIZE 64ULL
#define SAME_L1_DATA_SET_STRIDE (256ULL * CACHE_LINE_SIZE)
#define SAME_L2_SET_STRIDE (8192ULL * CACHE_LINE_SIZE)

typedef struct {
	uint64_t l1d_accesses;
	uint64_t l1d_misses;
	uint64_t l2_accesses;
	uint64_t l2_misses;
} policy_result_t;

static void expect(int condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "TEST FAILED: %s\n", message);
		exit(EXIT_FAILURE);
	}
}

static uint8_t line_offset(uint64_t mem_addr) {
	return (uint8_t)(mem_addr & (CACHE_LINE_SIZE - 1ULL));
}

static void seed_line(uint64_t base_addr, uint8_t seed) {
	uint64_t aligned_base = base_addr & ~(CACHE_LINE_SIZE - 1ULL);
	uint64_t i;

	for (i = 0; i < CACHE_LINE_SIZE; i++) {
		write_memory(aligned_base + i, (uint8_t)(seed + i));
	}
}

static policy_result_t snapshot_policy_result(void) {
	cache_stats_t l1d_stats = get_l1_data_stats();
	cache_stats_t l2_stats = get_l2_stats();
	policy_result_t result;

	result.l1d_accesses = l1d_stats.accesses;
	result.l1d_misses = l1d_stats.misses;
	result.l2_accesses = l2_stats.accesses;
	result.l2_misses = l2_stats.misses;
	return result;
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

static void test_basic_instr_hits(void) {
	uint64_t addr = 0x2000ULL + 11ULL;
	cache_stats_t l1_stats;
	cache_stats_t l2_stats;

	seed_line(addr, 0x30U);
	init_cache(LRU);

	expect(read_cache(addr, INSTR) == 0x3BU, "first INSTR read should come from memory");
	l1_stats = get_l1_instr_stats();
	l2_stats = get_l2_stats();
	expect(l1_stats.accesses == 1 && l1_stats.misses == 1, "first INSTR read should miss in L1-I");
	expect(l2_stats.accesses == 1 && l2_stats.misses == 1, "first INSTR read should miss in L2");
	expect(get_l1_instr_cache_line(addr) != NULL, "line should be installed in L1-I after miss");

	expect(read_cache(addr, INSTR) == 0x3BU, "second INSTR read should hit in L1-I");
	l1_stats = get_l1_instr_stats();
	l2_stats = get_l2_stats();
	expect(l1_stats.accesses == 2 && l1_stats.misses == 1, "second INSTR read should hit in L1-I");
	expect(l2_stats.accesses == 1 && l2_stats.misses == 1, "second INSTR read should not touch L2");
}

static void test_write_hit_marks_line_dirty(void) {
	uint64_t addr = 0x3000ULL + 9ULL;
	uint8_t old_value;
	cache_line_t* line;

	seed_line(addr, 0x40U);
	old_value = read_memory(addr);
	init_cache(LRU);

	(void)read_cache(addr, DATA);
	write_cache(addr, 0xD2U, DATA);

	line = get_l1_data_cache_line(addr);
	expect(line != NULL, "write-hit line should stay in L1-D");
	expect(line->modified == 1, "write hit should mark the L1-D line dirty");
	expect(line->data[line_offset(addr)] == 0xD2U, "write hit should update the cached byte");
	expect(read_cache(addr, DATA) == 0xD2U, "later read should return the updated byte");
	expect(read_memory(addr) == old_value, "write-back cache should not update memory on a write hit");
}

static void test_write_miss_uses_write_allocate(void) {
	uint64_t addr = 0x3800ULL + 5ULL;
	uint8_t old_value;
	cache_stats_t l1_stats;
	cache_stats_t l2_stats;
	cache_line_t* line;

	seed_line(addr, 0x50U);
	old_value = read_memory(addr);
	init_cache(LRU);

	write_cache(addr, 0xBEU, DATA);
	l1_stats = get_l1_data_stats();
	l2_stats = get_l2_stats();
	line = get_l1_data_cache_line(addr);

	expect(line != NULL, "write miss should allocate a line in L1-D");
	expect(line->modified == 1, "write miss should leave the allocated line dirty");
	expect(line->data[line_offset(addr)] == 0xBEU, "write miss should update the cached byte");
	expect(l1_stats.accesses == 1 && l1_stats.misses == 1, "write miss should count as one L1-D miss");
	expect(l2_stats.accesses == 1 && l2_stats.misses == 1, "write-allocate miss should fetch the line through L2");
	expect(read_memory(addr) == old_value, "write miss should not update memory immediately");
}

static void test_dirty_l1d_eviction_writes_back_to_l2(void) {
	uint64_t addr_a = 0x5000ULL + 1ULL;
	uint64_t addr_b = addr_a + SAME_L1_DATA_SET_STRIDE;
	uint64_t addr_c = addr_a + (2ULL * SAME_L1_DATA_SET_STRIDE);
	uint8_t old_value = 0;
	cache_line_t* l2_line;

	seed_line(addr_a, 0x60U);
	seed_line(addr_b, 0x70U);
	seed_line(addr_c, 0x80U);
	old_value = read_memory(addr_a);
	init_cache(LRU);

	(void)read_cache(addr_a, DATA);
	write_cache(addr_a, 0xE1U, DATA);
	(void)read_cache(addr_b, DATA);
	(void)read_cache(addr_c, DATA);

	expect(get_l1_data_cache_line(addr_a) == NULL, "conflicting access should evict the dirty line from L1-D");
	l2_line = get_l2_cache_line(addr_a);
	expect(l2_line != NULL, "dirty L1-D eviction should leave the line in L2");
	expect(l2_line->data[line_offset(addr_a)] == 0xE1U, "L2 should receive the updated byte from L1-D");
	expect(read_memory(addr_a) == old_value, "dirty L1-D eviction should not write to memory yet");
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
	cache_stats_t l2_stats;
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

	l2_stats = get_l2_stats();
	expect(get_l2_cache_line(victim_addr) == NULL, "victim line should be evicted from L2");
	expect(get_l1_instr_cache_line(victim_addr) == NULL, "L2 eviction should invalidate the inclusive L1-I copy");
	expect(l2_stats.accesses == 6 && l2_stats.misses == 5, "dirty L1-I backinvalidation should add one L2 access");
	expect(read_memory(victim_addr) == 0xEEU, "dirty L1-I data should reach memory on L2 eviction");
}

static void test_backinvalidate_dirty_l1_data(void) {
	uint64_t victim_addr = 0x18000ULL + 9ULL;
	uint64_t conflict_addrs[4];
	cache_stats_t l2_stats;
	int i;

	seed_line(victim_addr, 0x22U);
	for (i = 0; i < 4; i++) {
		conflict_addrs[i] = victim_addr + ((uint64_t)(i + 1) * SAME_L2_SET_STRIDE);
		seed_line(conflict_addrs[i], (uint8_t)(0x70U + (uint8_t)(i * 0x10U)));
	}

	init_cache(LRU);

	expect(read_cache(victim_addr, DATA) == 0x2BU, "victim data line should be loaded into L1-D");
	write_cache(victim_addr, 0xC3U, DATA);

	for (i = 0; i < 4; i++) {
		(void)read_cache(conflict_addrs[i], INSTR);
	}

	l2_stats = get_l2_stats();
	expect(get_l2_cache_line(victim_addr) == NULL, "dirty L1-D victim should be evicted from L2");
	expect(get_l1_data_cache_line(victim_addr) == NULL, "L2 eviction should invalidate the inclusive L1-D copy");
	expect(l2_stats.accesses == 6 && l2_stats.misses == 5, "dirty L1-D backinvalidation should add one L2 access");
	expect(read_memory(victim_addr) == 0xC3U, "dirty L1-D data should reach memory on L2 eviction");
}

static void test_l1d_set_associativity(void) {
	uint64_t addr_a = 0x1C000ULL + 2ULL;
	uint64_t addr_b = addr_a + SAME_L1_DATA_SET_STRIDE;
	uint64_t addr_c = addr_a + (2ULL * SAME_L1_DATA_SET_STRIDE);
	int resident_old_lines = 0;

	seed_line(addr_a, 0x11U);
	seed_line(addr_b, 0x21U);
	seed_line(addr_c, 0x31U);
	init_cache(LRU);

	(void)read_cache(addr_a, DATA);
	(void)read_cache(addr_b, DATA);
	expect(get_l1_data_cache_line(addr_a) != NULL, "first line should occupy one L1-D way");
	expect(get_l1_data_cache_line(addr_b) != NULL, "second line should occupy the other L1-D way");

	(void)read_cache(addr_c, DATA);
	if (get_l1_data_cache_line(addr_a) != NULL) {
		resident_old_lines++;
	}
	if (get_l1_data_cache_line(addr_b) != NULL) {
		resident_old_lines++;
	}

	expect(get_l1_data_cache_line(addr_c) != NULL, "third conflicting line should be installed");
	expect(resident_old_lines == 1, "exactly one older line should remain after a 2-way replacement");
}

static void test_init_cache_resets_state(void) {
	uint64_t addr = 0x24000ULL + 6ULL;

	seed_line(addr, 0x44U);
	init_cache(LRU);
	(void)read_cache(addr, DATA);
	write_cache(addr, 0xA1U, DATA);
	expect(get_l1_data_stats().accesses > 0, "setup should produce nonzero stats");
	expect(get_l1_data_cache_line(addr) != NULL, "setup should populate L1-D");

	init_cache(RANDOM);
	expect(get_l1_data_stats().accesses == 0 && get_l1_data_stats().misses == 0, "init_cache should reset L1-D stats");
	expect(get_l1_instr_stats().accesses == 0 && get_l1_instr_stats().misses == 0, "init_cache should reset L1-I stats");
	expect(get_l2_stats().accesses == 0 && get_l2_stats().misses == 0, "init_cache should reset L2 stats");
	expect(get_l1_data_cache_line(addr) == NULL, "init_cache should clear L1-D contents");
	expect(get_l1_instr_cache_line(addr) == NULL, "init_cache should clear L1-I contents");
	expect(get_l2_cache_line(addr) == NULL, "init_cache should clear L2 contents");
}

static void run_unit_tests(void) {
	test_basic_data_hits();
	test_basic_instr_hits();
	test_write_hit_marks_line_dirty();
	test_write_miss_uses_write_allocate();
	test_dirty_l1d_eviction_writes_back_to_l2();
	test_l1_coherency();
	test_inclusive_dirty_l2_eviction();
	test_backinvalidate_dirty_l1_data();
	test_l1d_set_associativity();
	test_init_cache_resets_state();
	printf("Unit tests passed.\n");
}

static policy_result_t run_conflict_thrash_workload(replacement_policy_e policy) {
	uint64_t base_addr = 0x12000ULL;
	uint64_t addr_a = base_addr;
	uint64_t addr_b = base_addr + SAME_L1_DATA_SET_STRIDE;
	uint64_t addr_c = base_addr + (2ULL * SAME_L1_DATA_SET_STRIDE);
	int iter;

	seed_line(addr_a, 0x60U);
	seed_line(addr_b, 0x70U);
	seed_line(addr_c, 0x80U);
	init_cache(policy);

	for (iter = 0; iter < 2000; iter++) {
		(void)read_cache(addr_a, DATA);
		(void)read_cache(addr_b, DATA);
		(void)read_cache(addr_c, DATA);
	}

	return snapshot_policy_result();
}

static policy_result_t run_hot_set_intruder_workload(replacement_policy_e policy) {
	uint64_t base_addr = 0x32000ULL;
	uint64_t addr_a = base_addr;
	uint64_t addr_b = base_addr + SAME_L1_DATA_SET_STRIDE;
	uint64_t addr_c = base_addr + (2ULL * SAME_L1_DATA_SET_STRIDE);
	int iter;

	seed_line(addr_a, 0x15U);
	seed_line(addr_b, 0x25U);
	seed_line(addr_c, 0x35U);
	init_cache(policy);

	(void)read_cache(addr_a, DATA);
	(void)read_cache(addr_b, DATA);
	for (iter = 0; iter < 5000; iter++) {
		(void)read_cache(addr_a, DATA);
		(void)read_cache(addr_b, DATA);
		(void)read_cache(addr_a, DATA);
		(void)read_cache(addr_b, DATA);
		(void)read_cache(addr_a, DATA);
		(void)read_cache(addr_c, DATA);
	}

	return snapshot_policy_result();
}

static void print_policy_result(const char* policy_name, policy_result_t result) {
	printf("  %s: L1-D accesses=%llu misses=%llu | L2 accesses=%llu misses=%llu\n",
	       policy_name,
	       (unsigned long long)result.l1d_accesses,
	       (unsigned long long)result.l1d_misses,
	       (unsigned long long)result.l2_accesses,
	       (unsigned long long)result.l2_misses);
}

static void run_policy_comparison_report(void) {
	policy_result_t thrash_lru = run_conflict_thrash_workload(LRU);
	policy_result_t thrash_random = run_conflict_thrash_workload(RANDOM);
	policy_result_t hot_lru = run_hot_set_intruder_workload(LRU);
	policy_result_t hot_random = run_hot_set_intruder_workload(RANDOM);

	expect(thrash_lru.l1d_misses > thrash_random.l1d_misses,
	       "cyclic thrash workload should favor RANDOM over LRU in this local report");
	expect(hot_lru.l1d_misses < hot_random.l1d_misses,
	       "hot-set-with-intruder workload should favor LRU over RANDOM in this local report");

	printf("Replacement policy comparison report:\n");
	printf("Conflict thrash workload (A, B, C repeated in one 2-way L1-D set):\n");
	print_policy_result("LRU", thrash_lru);
	print_policy_result("RANDOM", thrash_random);
	printf("Hot-set workload (A and B hot, C occasional intruder in the same set):\n");
	print_policy_result("LRU", hot_lru);
	print_policy_result("RANDOM", hot_random);
}

static void print_usage(const char* program_name) {
	printf("Usage: %s [all|unit|compare]\n", program_name);
	printf("  all     run the unit-test suite and the policy-comparison report (default)\n");
	printf("  unit    run only the unit-test suite\n");
	printf("  compare run only the replacement-policy comparison report\n");
}

int main(int argc, char* argv[]) {
	if (argc == 1 || strcmp(argv[1], "all") == 0) {
		run_unit_tests();
		run_policy_comparison_report();
		return 0;
	}

	if (strcmp(argv[1], "unit") == 0) {
		run_unit_tests();
		return 0;
	}

	if (strcmp(argv[1], "compare") == 0) {
		run_policy_comparison_report();
		return 0;
	}

	print_usage(argv[0]);
	return EXIT_FAILURE;
}
