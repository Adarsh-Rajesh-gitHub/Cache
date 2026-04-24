#include "tcache.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CACHE_LINE_SIZE 64ULL
#define OFFSET_BITS 6U

#define L1_INST_SET_COUNT (HW11_L1_SIZE / CACHE_LINE_SIZE / HW11_L1_INSTR_ASSOC)
#define L1_DATA_SET_COUNT (HW11_L1_SIZE / CACHE_LINE_SIZE / HW11_L1_DATA_ASSOC)
#define L2_SET_COUNT (HW11_L2_SIZE / CACHE_LINE_SIZE / HW11_L2_ASSOC)

#define L1_INST_INDEX_BITS 9U
#define L1_DATA_INDEX_BITS 8U
#define L2_INDEX_BITS 13U

replacement_policy_e pol = LRU;
static cache_line_t L1_inst[L1_INST_SET_COUNT];
static cache_line_t L1_data[L1_DATA_SET_COUNT][HW11_L1_DATA_ASSOC];
static cache_line_t L2[L2_SET_COUNT][HW11_L2_ASSOC];

static uint64_t L1_inst_lru[L1_INST_SET_COUNT];
static uint64_t L1_data_lru[L1_DATA_SET_COUNT][HW11_L1_DATA_ASSOC];
static uint64_t L2_lru[L2_SET_COUNT][HW11_L2_ASSOC];

static cache_stats_t l1_instr_stats;
static cache_stats_t l1_data_stats;
static cache_stats_t l2_stats;

static uint64_t access_clock = 0;
static uint32_t random_state = 0x12345678U;

static uint64_t line_base_addr(uint64_t mem_addr) {
	return mem_addr & ~(CACHE_LINE_SIZE - 1ULL);
}

static uint64_t build_mem_addr(uint64_t tag, uint64_t index, unsigned index_bits) {
	return (tag << (index_bits + OFFSET_BITS)) | (index << OFFSET_BITS);
}

static uint64_t l1_instr_index(uint64_t mem_addr) {
	return (mem_addr >> OFFSET_BITS) & (L1_INST_SET_COUNT - 1ULL);
}

static uint64_t l1_data_index(uint64_t mem_addr) {
	return (mem_addr >> OFFSET_BITS) & (L1_DATA_SET_COUNT - 1ULL);
}

static uint64_t l2_index(uint64_t mem_addr) {
	return (mem_addr >> OFFSET_BITS) & (L2_SET_COUNT - 1ULL);
}

static uint64_t l1_instr_tag(uint64_t mem_addr) {
	return mem_addr >> (OFFSET_BITS + L1_INST_INDEX_BITS);
}

static uint64_t l1_data_tag(uint64_t mem_addr) {
	return mem_addr >> (OFFSET_BITS + L1_DATA_INDEX_BITS);
}

static uint64_t l2_tag(uint64_t mem_addr) {
	return mem_addr >> (OFFSET_BITS + L2_INDEX_BITS);
}

static uint32_t next_random(void) {
	random_state = (random_state * 1664525U) + 1013904223U;
	return random_state;
}

static void touch_l1_inst(uint64_t set_index) {
	L1_inst_lru[set_index] = ++access_clock;
}

static void touch_l1_data(uint64_t set_index, int way) {
	L1_data_lru[set_index][way] = ++access_clock;
}

static void touch_l2(uint64_t set_index, int way) {
	L2_lru[set_index][way] = ++access_clock;
}

static void load_line_from_memory(cache_line_t* line, uint64_t mem_addr, uint64_t tag) {
	uint64_t base_addr = line_base_addr(mem_addr);
	size_t i;

	line->valid = 1;
	line->modified = 0;
	line->tag = tag;
	for (i = 0; i < sizeof(line->data); i++) {
		line->data[i] = read_memory(base_addr + i);
	}
}

static void write_line_to_memory(const cache_line_t* line, uint64_t base_addr) {
	size_t i;

	for (i = 0; i < sizeof(line->data); i++) {
		write_memory(base_addr + i, line->data[i]);
	}
}

static cache_line_t* find_l1_instr_line_internal(uint64_t mem_addr) {
	uint64_t set_index = l1_instr_index(mem_addr);
	cache_line_t* line = &L1_inst[set_index];

	if (line->valid && line->tag == l1_instr_tag(mem_addr)) {
		return line;
	}
	return NULL;
}

static int find_l1_data_way(uint64_t mem_addr) {
	uint64_t set_index = l1_data_index(mem_addr);
	uint64_t tag = l1_data_tag(mem_addr);
	int way;

	for (way = 0; way < HW11_L1_DATA_ASSOC; way++) {
		if (L1_data[set_index][way].valid && L1_data[set_index][way].tag == tag) {
			return way;
		}
	}
	return -1;
}

static cache_line_t* find_l1_data_line_internal(uint64_t mem_addr) {
	uint64_t set_index = l1_data_index(mem_addr);
	int way = find_l1_data_way(mem_addr);

	if (way < 0) {
		return NULL;
	}
	return &L1_data[set_index][way];
}

static int find_l2_way(uint64_t mem_addr) {
	uint64_t set_index = l2_index(mem_addr);
	uint64_t tag = l2_tag(mem_addr);
	int way;

	for (way = 0; way < HW11_L2_ASSOC; way++) {
		if (L2[set_index][way].valid && L2[set_index][way].tag == tag) {
			return way;
		}
	}
	return -1;
}

static cache_line_t* find_l2_line_internal(uint64_t mem_addr) {
	uint64_t set_index = l2_index(mem_addr);
	int way = find_l2_way(mem_addr);

	if (way < 0) {
		return NULL;
	}
	return &L2[set_index][way];
}

static int choose_l1_data_victim(uint64_t set_index) {
	int way;
	int victim = 0;

	for (way = 0; way < HW11_L1_DATA_ASSOC; way++) {
		if (!L1_data[set_index][way].valid) {
			return way;
		}
	}
	if (pol == RANDOM) {
		return (int)(next_random() % HW11_L1_DATA_ASSOC);
	}
	for (way = 1; way < HW11_L1_DATA_ASSOC; way++) {
		if (L1_data_lru[set_index][way] < L1_data_lru[set_index][victim]) {
			victim = way;
		}
	}
	return victim;
}

static int choose_l2_victim(uint64_t set_index) {
	int way;
	int victim = 0;

	for (way = 0; way < HW11_L2_ASSOC; way++) {
		if (!L2[set_index][way].valid) {
			return way;
		}
	}
	if (pol == RANDOM) {
		return (int)(next_random() % HW11_L2_ASSOC);
	}
	for (way = 1; way < HW11_L2_ASSOC; way++) {
		if (L2_lru[set_index][way] < L2_lru[set_index][victim]) {
			victim = way;
		}
	}
	return victim;
}

static void invalidate_l1_copies(uint64_t mem_addr) {
	cache_line_t* inst_line = find_l1_instr_line_internal(mem_addr);
	cache_line_t* data_line = find_l1_data_line_internal(mem_addr);

	if (inst_line != NULL) {
		inst_line->valid = 0;
		inst_line->modified = 0;
	}
	if (data_line != NULL) {
		data_line->valid = 0;
		data_line->modified = 0;
	}
}

static void write_back_l1_line_to_l2(cache_line_t* line, uint64_t mem_addr);

static void evict_l2_line(uint64_t set_index, int way) {
	cache_line_t* line = &L2[set_index][way];
	uint64_t evicted_addr;
	cache_line_t* inst_line;
	cache_line_t* data_line;

	if (!line->valid) {
		return;
	}

	evicted_addr = build_mem_addr(line->tag, set_index, L2_INDEX_BITS);
	inst_line = find_l1_instr_line_internal(evicted_addr);
	data_line = find_l1_data_line_internal(evicted_addr);

	// Back-invalidating a dirty L1 copy still counts as an L1 writeback into L2.
	write_back_l1_line_to_l2(inst_line, evicted_addr);
	write_back_l1_line_to_l2(data_line, evicted_addr);

	if (line->modified) {
		write_line_to_memory(line, evicted_addr);
	}

	invalidate_l1_copies(evicted_addr);
	line->valid = 0;
	line->modified = 0;
	line->tag = 0;
}

static cache_line_t* access_l2_line(uint64_t mem_addr) {
	uint64_t set_index = l2_index(mem_addr);
	uint64_t tag = l2_tag(mem_addr);
	int way;

	l2_stats.accesses++;
	way = find_l2_way(mem_addr);
	if (way >= 0) {
		touch_l2(set_index, way);
		return &L2[set_index][way];
	}

	l2_stats.misses++;
	way = choose_l2_victim(set_index);
	evict_l2_line(set_index, way);
	load_line_from_memory(&L2[set_index][way], mem_addr, tag);
	touch_l2(set_index, way);
	return &L2[set_index][way];
}

static void write_back_l1_line_to_l2(cache_line_t* line, uint64_t mem_addr) {
	uint64_t set_index;
	uint64_t tag;
	int way;

	if (line == NULL || !line->valid || !line->modified) {
		return;
	}

	set_index = l2_index(mem_addr);
	tag = l2_tag(mem_addr);
	l2_stats.accesses++;

	way = find_l2_way(mem_addr);
	if (way < 0) {
		l2_stats.misses++;
		way = choose_l2_victim(set_index);
		evict_l2_line(set_index, way);
		L2[set_index][way].valid = 1;
		L2[set_index][way].tag = tag;
	}

	L2[set_index][way].valid = 1;
	L2[set_index][way].modified = 1;
	L2[set_index][way].tag = tag;
	memcpy(L2[set_index][way].data, line->data, sizeof(line->data));
	touch_l2(set_index, way);
	line->modified = 0;
}

static void evict_l1_instr_line(uint64_t set_index) {
	cache_line_t* line = &L1_inst[set_index];
	uint64_t mem_addr;

	if (!line->valid) {
		return;
	}

	mem_addr = build_mem_addr(line->tag, set_index, L1_INST_INDEX_BITS);
	write_back_l1_line_to_l2(line, mem_addr);
	line->valid = 0;
	line->modified = 0;
	line->tag = 0;
}

static void evict_l1_data_line(uint64_t set_index, int way) {
	cache_line_t* line = &L1_data[set_index][way];
	uint64_t mem_addr;

	if (!line->valid) {
		return;
	}

	mem_addr = build_mem_addr(line->tag, set_index, L1_DATA_INDEX_BITS);
	write_back_l1_line_to_l2(line, mem_addr);
	line->valid = 0;
	line->modified = 0;
	line->tag = 0;
}

static void ensure_peer_writeback(uint64_t mem_addr, mem_type_t type) {
	cache_line_t* peer_line = NULL;

	if (type == INSTR) {
		peer_line = find_l1_data_line_internal(mem_addr);
	} else {
		peer_line = find_l1_instr_line_internal(mem_addr);
	}

	if (peer_line != NULL && peer_line->modified) {
		write_back_l1_line_to_l2(peer_line, mem_addr);
	}
}

static void invalidate_peer_l1_line(uint64_t mem_addr, mem_type_t type) {
	cache_line_t* peer_line = NULL;

	if (type == INSTR) {
		peer_line = find_l1_data_line_internal(mem_addr);
	} else {
		peer_line = find_l1_instr_line_internal(mem_addr);
	}

	if (peer_line != NULL) {
		peer_line->valid = 0;
		peer_line->modified = 0;
	}
}

static cache_line_t* install_in_l1(uint64_t mem_addr, mem_type_t type, const cache_line_t* source_line) {
	uint64_t set_index;
	int way;

	if (type == INSTR) {
		set_index = l1_instr_index(mem_addr);
		evict_l1_instr_line(set_index);
		L1_inst[set_index].valid = 1;
		L1_inst[set_index].modified = 0;
		L1_inst[set_index].tag = l1_instr_tag(mem_addr);
		memcpy(L1_inst[set_index].data, source_line->data, sizeof(source_line->data));
		touch_l1_inst(set_index);
		return &L1_inst[set_index];
	}

	set_index = l1_data_index(mem_addr);
	way = choose_l1_data_victim(set_index);
	evict_l1_data_line(set_index, way);
	L1_data[set_index][way].valid = 1;
	L1_data[set_index][way].modified = 0;
	L1_data[set_index][way].tag = l1_data_tag(mem_addr);
	memcpy(L1_data[set_index][way].data, source_line->data, sizeof(source_line->data));
	touch_l1_data(set_index, way);
	return &L1_data[set_index][way];
}

static cache_line_t* resolve_l1_line_for_access(uint64_t mem_addr, mem_type_t type) {
	cache_line_t* line;
	cache_line_t* l2_line;
	uint64_t set_index;
	int way;

	if (type == INSTR) {
		line = find_l1_instr_line_internal(mem_addr);
		if (line != NULL) {
			touch_l1_inst(l1_instr_index(mem_addr));
			return line;
		}
	} else {
		line = find_l1_data_line_internal(mem_addr);
		if (line != NULL) {
			set_index = l1_data_index(mem_addr);
			way = find_l1_data_way(mem_addr);
			touch_l1_data(set_index, way);
			return line;
		}
	}

	ensure_peer_writeback(mem_addr, type);
	l2_line = access_l2_line(mem_addr);
	return install_in_l1(mem_addr, type, l2_line);
}

// STUDENT TODO: initialize cache with replacement policy
void init_cache(replacement_policy_e policy) {
	pol = policy;
	memset(L1_inst, 0, sizeof(L1_inst));
	memset(L1_data, 0, sizeof(L1_data));
	memset(L2, 0, sizeof(L2));
	memset(L1_inst_lru, 0, sizeof(L1_inst_lru));
	memset(L1_data_lru, 0, sizeof(L1_data_lru));
	memset(L2_lru, 0, sizeof(L2_lru));
	memset(&l1_instr_stats, 0, sizeof(l1_instr_stats));
	memset(&l1_data_stats, 0, sizeof(l1_data_stats));
	memset(&l2_stats, 0, sizeof(l2_stats));
	access_clock = 0;
	random_state = 0x12345678U;
}

// STUDENT TODO: implement read cache, using the l1 and l2 structure
uint8_t read_cache(uint64_t mem_addr, mem_type_t type) {
	// mem_addr follows the format (MSB) tag | index | offset (LSB)
	uint8_t offset = (uint8_t)(mem_addr & (CACHE_LINE_SIZE - 1ULL));
	cache_line_t* line;

	if (type == INSTR) {
		l1_instr_stats.accesses++;
		if (find_l1_instr_line_internal(mem_addr) == NULL) {
			l1_instr_stats.misses++;
		}
	} else {
		l1_data_stats.accesses++;
		if (find_l1_data_line_internal(mem_addr) == NULL) {
			l1_data_stats.misses++;
		}
	}

	line = resolve_l1_line_for_access(mem_addr, type);
	return line->data[offset];
}

// STUDENT TODO: implement write cache, using the l1 and l2 structure
void write_cache(uint64_t mem_addr, uint8_t value, mem_type_t type) {
	uint8_t offset = (uint8_t)(mem_addr & (CACHE_LINE_SIZE - 1ULL));
	cache_line_t* line;

	if (type == INSTR) {
		l1_instr_stats.accesses++;
		if (find_l1_instr_line_internal(mem_addr) == NULL) {
			l1_instr_stats.misses++;
		}
	} else {
		l1_data_stats.accesses++;
		if (find_l1_data_line_internal(mem_addr) == NULL) {
			l1_data_stats.misses++;
		}
	}

	line = resolve_l1_line_for_access(mem_addr, type);
	line->data[offset] = value;
	line->modified = 1;
	invalidate_peer_l1_line(mem_addr, type);
}

// STUDENT TODO: implement functions to get cache stats
cache_stats_t get_l1_instr_stats() {
	return l1_instr_stats;
}

cache_stats_t get_l1_data_stats() {
	return l1_data_stats;
}

cache_stats_t get_l2_stats() {
	return l2_stats;
}

// STUDENT TODO: implement a function returning a pointer to a specific cache line for an address
//               or null if the line is not present in the cache
cache_line_t* get_l1_instr_cache_line(uint64_t mem_addr) {
	return find_l1_instr_line_internal(mem_addr);
}

cache_line_t* get_l1_data_cache_line(uint64_t mem_addr) {
	return find_l1_data_line_internal(mem_addr);
}

cache_line_t* get_l2_cache_line(uint64_t mem_addr) {
	return find_l2_line_internal(mem_addr);
}
