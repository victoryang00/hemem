/*
 * memsim - memory system emulator
 *
 * Caveats:
 * L123 caches are not emulated.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "shared.h"

struct pte	*cr3 = NULL;
_Atomic size_t	runtime = 0;		// Elapsed simulation time

// Hardware 2-level TLB emulating Cascade Lake
struct tlbe {
  uint64_t	vpfn, ppfn;
  bool		present;
};

static struct tlbe l1tlb_1g[4], l1tlb_2m[32], l1tlb_4k[64];
static struct tlbe l2tlb_1g[16], l2tlb_2m4k[1536];

// Statistics
static size_t accesses[NMEMTYPES], tlbmisses = 0, tlbhits = 0, pagefaults = 0;

#ifdef MMM
#	define	MMM_TAGS_SIZE	(FASTMEM_SIZE / CACHELINE_SIZE)

static uint64_t	mmm_tags[MMM_TAGS_SIZE];
#endif

// From Wikipedia
static uint32_t jenkins_one_at_a_time_hash(const uint8_t *key, size_t length) {
  size_t i = 0;
  uint32_t hash = 0;

  while (i != length) {
    hash += key[i++];
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  
  return hash;
}

static unsigned int tlb_hash(uint64_t addr)
{
  return jenkins_one_at_a_time_hash((uint8_t *)&addr, sizeof(uint64_t));
}

void tlb_shootdown(uint64_t addr)
{
  memset(l1tlb_1g, 0, sizeof(l1tlb_1g));
  memset(l1tlb_2m, 0, sizeof(l1tlb_2m));
  memset(l1tlb_4k, 0, sizeof(l1tlb_4k));
  memset(l2tlb_1g, 0, sizeof(l2tlb_1g));
  memset(l2tlb_2m4k, 0, sizeof(l2tlb_2m4k));

  runtime += TIME_TLBSHOOTDOWN;
}

static struct tlbe *tlb_lookup(struct tlbe *tlb, unsigned int size,
			       uint64_t vpfn)
{
  struct tlbe *te = &tlb[tlb_hash(vpfn) % size];
  if(te->present && te->vpfn == vpfn) {
    return te;
  } else {
    return NULL;
  }
}

static void tlb_insert(uint64_t vaddr, uint64_t paddr, unsigned int level)
{
  struct tlbe *te;
  uint64_t vpfn = 0, ppfn = 0;

  assert(level >= 2 && level <= 4);
  
  switch(level) {
  case 2:	// 1GB page
    vpfn = vaddr & GIGA_PFN_MASK;
    ppfn = paddr & GIGA_PFN_MASK;
    te = &l1tlb_1g[tlb_hash(vpfn) % 4];
    if(te->present) {
      // Move previous entry down
      assert(te->vpfn != vpfn);
      memcpy(&l2tlb_1g[tlb_hash(vpfn) % 16], te, sizeof(struct tlbe));
    }
    break;

  case 3:	// 2MB page
    vpfn = vaddr & HUGE_PFN_MASK;
    ppfn = paddr & HUGE_PFN_MASK;
    te = &l1tlb_2m[tlb_hash(vpfn) % 32];

    // Fall through...
  case 4:	// 4KB page
    if(level == 4) {
      vpfn = vaddr & BASE_PFN_MASK;
      ppfn = paddr & BASE_PFN_MASK;
      te = &l1tlb_4k[tlb_hash(vpfn) % 64];
    }
    if(te->present) {
      // Move previous entry down
      assert(te->vpfn != vpfn);
      memcpy(&l2tlb_2m4k[tlb_hash(vpfn) % 1536], te, sizeof(struct tlbe));
    }
    break;
  }

  te->present = true;
  te->vpfn = vpfn;
  te->ppfn = ppfn;
}

static void memaccess(uint64_t addr, enum access_type type)
{
  int level = 2;

  // Must be canonical addr
  assert((addr >> 48) == 0);

  // In TLB?
  struct tlbe *te = NULL;
  uint64_t paddr;
  if((te = tlb_lookup(l1tlb_1g, 4, addr & GIGA_PFN_MASK)) == NULL && (level = 3) &&
     (te = tlb_lookup(l1tlb_2m, 32, addr & HUGE_PFN_MASK)) == NULL && (level = 4) &&
     (te = tlb_lookup(l1tlb_4k, 64, addr & BASE_PFN_MASK)) == NULL && (level = 2) &&
     (te = tlb_lookup(l2tlb_1g, 16, addr & GIGA_PFN_MASK)) == NULL && (level = 3) &&
     (te = tlb_lookup(l2tlb_2m4k, 1536, addr & HUGE_PFN_MASK)) == NULL && (level = 4) &&
     (te = tlb_lookup(l2tlb_2m4k, 1536, addr & BASE_PFN_MASK)) == NULL) {
    tlbmisses++;

    // 4-level page walk
    assert(cr3 != NULL);
    struct pte *ptable = cr3, *pte = NULL;

    for(level = 1; level <= 4 && ptable != NULL; level++) {
      pte = &ptable[(addr >> (48 - (level * 9))) & 511];

      runtime += TIME_PAGEWALK;

      if(!pte->present) {
	pagefault(addr);
	runtime += TIME_PAGEFAULT;
	pagefaults++;
	assert(pte->present);
      }

      pte->accessed = true;
      if(type == TYPE_WRITE) {
	pte->modified = true;
      }

      if(pte->pagemap) {
	// Page here -- terminate walk
	break;
      }
      
      ptable = pte->next;
    }

    assert(pte != NULL);
    assert(level >= 2 && level <= 4);
    paddr = pte->addr + (addr & ((1 << (12 + (4 - level) * 9)) - 1));

    // Insert in TLB
    tlb_insert(addr, paddr, level);
  } else {
    tlbhits++;
    paddr = te->ppfn + (addr & ((1 << (12 + (4 - level) * 9)) - 1));
  }

#ifdef MMM
  assert(MMM_TAGS_SIZE <= UINT32_MAX);

  uint64_t cline = paddr / CACHELINE_SIZE;
  unsigned int mmm_idx = tlb_hash(cline) % MMM_TAGS_SIZE;
  bool in_fastmem = mmm_tags[mmm_idx] == cline ? true : false;

  // MMM miss? Write back and (maybe) load new
  if(!in_fastmem) {
    if(mmm_tags[mmm_idx] != (uint64_t)-1) {
      runtime += TIME_SLOWMEM_WRITE;	// Write back
      accesses[SLOWMEM]++;
    }
    if(type == TYPE_READ) {
      runtime += TIME_SLOWMEM_READ;	// Load new
      accesses[SLOWMEM]++;
    }
    
    mmm_tags[mmm_idx] = cline;
  }

  runtime += (type == TYPE_READ) ? TIME_FASTMEM_READ : TIME_FASTMEM_WRITE;
  accesses[FASTMEM]++;
#else
  if(type == TYPE_READ) {
    runtime += (paddr & SLOWMEM_BIT) ? TIME_SLOWMEM_READ : TIME_FASTMEM_READ;
  } else {
    runtime += (paddr & SLOWMEM_BIT) ? TIME_SLOWMEM_WRITE : TIME_FASTMEM_WRITE;
  }

  accesses[(paddr & SLOWMEM_BIT) ? SLOWMEM : FASTMEM]++;
#endif

  /* if((pte->addr & SLOWMEM_BIT) && type == TYPE_READ) { */
  /*   LOG("%zu memaccess %s %" PRIu64 " %s %" PRIu64 " %d\n", */
  /* 	runtime, */
  /* 	type == TYPE_READ ? "read" : "write", */
  /* 	addr, */
  /* 	(pte->addr & SLOWMEM_BIT) ? "slow" : "fast", */
  /* 	(pte->addr & SLOWMEM_MASK) / BASE_PAGE_SIZE, */
  /* 	listnum(pte)); */
  /* } */
}

#define WORKSET_SIZE	MB(10)

#define RAND_WITHIN(x)	(((double)rand() / RAND_MAX) * (x))

static void gups(size_t iters, uint64_t hotset_start, uint64_t hotset_size,
		 double hotset_prob)
{
  // GUPS with hotset
  for(size_t i = 0; i < iters; i++) {
    uint64_t a;

    if(RAND_WITHIN(1) < hotset_prob) {
      // Hot set
      a = hotset_start + (uint64_t)RAND_WITHIN(hotset_size);
    } else {
      // Entire working set
      a = (uint64_t)RAND_WITHIN(WORKSET_SIZE);
    }

    // Read&update
    memaccess(a, TYPE_READ);
    memaccess(a, TYPE_WRITE);

    runtime += 100;	// 100ns program time per update
  }
}

static void reset_stats(void)
{
  LOG("%zu --- reset_stats ---\n", runtime);

  runtime = 0;
  accesses[FASTMEM] = accesses[SLOWMEM] = 0;
  tlbmisses = tlbhits = 0;
  pagefaults = 0;
}

int main(int argc, char *argv[])
{
#ifdef MMM
  // Clear MMM tags
  for(size_t i = 0; i < MMM_TAGS_SIZE; i++) {
    mmm_tags[i] = (uint64_t)-1;
  }
#endif

  mmgr_init();

  // Get memory traces from Onur's group at ETH? membench? Replay them here?

  // GUPS!
  gups(10000000, 0, MB(2), 0.9);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, tlbhits, pagefaults);

  reset_stats();

  // Move hotset up
  gups(10000000, MB(9), MB(2), 0.9);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, tlbhits, pagefaults);

  return 0;
}
