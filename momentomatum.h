// Cuckoo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2014 John Tromp
// The edge=trimming time-memory trade-off is due to Dave Anderson:
// http://da-data.blogspot.com/2014/03/a-public-review-of-cuckoo-cycle.html

#define PROOFSIZE 2
#include "cuckoo.h"
#ifdef __APPLE__
#include "osx_barrier.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#ifdef ATOMIC
#include <atomic>
typedef std::atomic<u32> au32;
typedef std::atomic<u64> au64;
#else
typedef u32 au32;
typedef u64 au64;
#endif
#include <set>

// algorithm parameters

#ifndef SAVEMEM_BITS
#define SAVEMEM_BITS 6
#endif

#ifndef IDXSHIFT
#define IDXSHIFT SAVEMEM_BITS
#endif
#ifndef LOGPROOFSIZE
// roughly the binary logarithm of cycle length rounded down
// tweak if necessary to get cuckoo hash load between 45 and 90%
#define LOGPROOFSIZE -2
#endif

// #partitions of vertex set
#ifndef NUPARTS
#define NUPARTS 64
#endif
#define PART_SIZE ((HALFSIZE+NUPARTS-1)/NUPARTS)

#define ONCE_BITS PART_SIZE
#define TWICE_WORDS ((2 * ONCE_BITS) / 32)

class twice_set {
public:
  au32 *bits;

  twice_set() {
    assert(bits = (au32 *)calloc(TWICE_WORDS, sizeof(au32)));
  }
  void reset() {
    memset(bits, 0, TWICE_WORDS*sizeof(au32));
  }
  void set(node_t u) {
    node_t idx = u/16;
    u32 bit = 1 << (2 * (u%16));
#ifdef ATOMIC
    u32 old = std::atomic_fetch_or_explicit(&bits[idx], bit, std::memory_order_relaxed);
    if (old & bit) std::atomic_fetch_or_explicit(&bits[idx], bit<<1, std::memory_order_relaxed);
  }
  u32 test(node_t u) const {
    return (bits[u/16].load(std::memory_order_relaxed) >> (2 * (u%16))) & 2;
  }
#else
    u32 old = bits[idx];
    bits[idx] = old | (bit + (old & bit));
  }
  u32 test(node_t u) const {
    return bits[u/16] >> (2 * (u%16)) & 2;
  }
#endif
  ~twice_set() {
    free(bits);
  }
};

#define UPART_MASK (NUPARTS - 1)
// grow with cube root of size, hardly affected by trimming
#define MAXPATHLEN 2

#define CUCKOO_SIZE (5<<16)
// number of (least significant) key bits that survives leftshift by SIZESHIFT
#define KEYBITS (64-SIZESHIFT)
#define KEYMASK ((1L << KEYBITS) - 1)

class cuckoo_hash {
public:
  au64 *cuckoo;
  au32 nstored;

  cuckoo_hash() {
    assert(cuckoo = (au64 *)calloc(CUCKOO_SIZE, sizeof(au64)));
  }
  ~cuckoo_hash() {
    free(cuckoo);
  }
  void clear() {
    memset(cuckoo, 0, CUCKOO_SIZE*sizeof(au64));
    nstored = 0;
  }
  void set(node_t u, node_t v) {
    u64 niew = (u64)u << SIZESHIFT | v;
    for (node_t ui = u % CUCKOO_SIZE; ; (++ui < CUCKOO_SIZE) || (ui = 0)) {
#ifdef ATOMIC
      u64 old = 0;
      if (cuckoo[ui].compare_exchange_strong(old, niew, std::memory_order_relaxed)) {
        std::atomic_fetch_add(&nstored, 1U);
        return;
      }
      if ((old >> SIZESHIFT) == (u & KEYMASK)) {
        cuckoo[ui].store(niew, std::memory_order_relaxed);
#else
      u64 old = cuckoo[ui];
      if ((old == 0 && ++nstored) || (old >> SIZESHIFT) == (u & KEYMASK)) {
        cuckoo[ui] = niew;
#endif
        return;
      }
    }
  }
  node_t operator[](node_t u) const {
    for (node_t ui = u % CUCKOO_SIZE; ; (++ui < CUCKOO_SIZE) || (ui = 0)) {
#ifdef ATOMIC
      u64 cu = cuckoo[ui].load(std::memory_order_relaxed);
#else
      u64 cu = cuckoo[ui];
#endif
      if (!cu)
        return 0;
      if ((cu >> SIZESHIFT) == (u & KEYMASK)) {
        return (node_t)(cu & (SIZE-1));
      }
    }
  }
  u32 load() const {
    return (u32)(nstored*100L/CUCKOO_SIZE);
  }
  bool overloaded() const {
    return nstored >= (u32)(CUCKOO_SIZE*9L/10L);
  }
};

class cuckoo_ctx {
public:
  siphash_ctx sip_ctx;
  cuckoo_hash *cuckoo;
  bool minimalbfs;
  twice_set *nonleaf;
  u32 nparts;
  nonce_t (*sols)[PROOFSIZE];
  u32 nthreads;
  pthread_barrier_t barry;

  cuckoo_ctx(const char* header, u32 n_threads, u32 n_parts, bool minimal_bfs) {
    setheader(&sip_ctx, header);
    nthreads = n_threads;
    nparts = n_parts;
    cuckoo = new cuckoo_hash();
    minimalbfs = minimal_bfs;
    nonleaf = new twice_set();
    assert(pthread_barrier_init(&barry, NULL, nthreads) == 0);
  }
  ~cuckoo_ctx() {
    delete cuckoo;
    delete nonleaf;
  }
};

typedef struct {
  u32 id;
  pthread_t thread;
  cuckoo_ctx *ctx;
} thread_ctx;

void barrier(pthread_barrier_t *barry) {
  int rc = pthread_barrier_wait(barry);
  if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
    printf("Could not wait on barrier\n");
    pthread_exit(NULL);
  }
}

typedef std::pair<node_t,node_t> edge;

void solution(cuckoo_ctx *ctx, node_t *us, u32 nu, node_t *vs, u32 nv) {
  std::set<edge> cycle;
  u32 n;
  cycle.insert(edge(*us, *vs));
  while (nu--)
    cycle.insert(edge(us[(nu+1)&~1], us[nu|1])); // u's in even position; v's in odd
  while (nv--)
    cycle.insert(edge(vs[nv|1], vs[(nv+1)&~1])); // u's in odd position; v's in even
  printf("Solution: ");
  for (nonce_t nonce = n = 0; nonce < HALFSIZE; nonce++) {
    node_t u,v;
    sipedge(&ctx->sip_ctx, nonce, &u, &v);
    edge e(u,v);
    if (cycle.find(e) != cycle.end()) {
      printf("%x%c", nonce, ++n == PROOFSIZE?'\n':' ');
      if (PROOFSIZE > 2)
        cycle.erase(e);
    }
  }
  assert(n==PROOFSIZE);
}

void *worker(void *vp) {
  thread_ctx *tp = (thread_ctx *)vp;
  cuckoo_ctx *ctx = tp->ctx;

  cuckoo_hash &cuckoo = *ctx->cuckoo;
  twice_set *nonleaf = ctx->nonleaf;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN];
  for (node_t upart=0; upart < ctx->nparts; upart++) {
    for (nonce_t nonce = tp->id; nonce < HALFSIZE; nonce += ctx->nthreads) {
      node_t u0 = sipnode(&ctx->sip_ctx, nonce, 0) >> 1;
      if (u0 != 0 && (u0 & UPART_MASK) == upart)
          nonleaf->set(u0 >> UPART_BITS);
    }
    barrier(&ctx->barry);
    static int bfsdepth = ctx->minimalbfs ? PROOFSIZE/2 : PROOFSIZE;
    for (int depth=0; depth < bfsdepth; depth++) {
      for (nonce_t nonce = tp->id; nonce < HALFSIZE; nonce += ctx->nthreads) {
        node_t u0 = sipnode(&ctx->sip_ctx, nonce, 0);
        if (u0 == 0)
          continue;
        node_t u1 = u0 >> 1;
        if ((u1 & UPART_MASK) != upart)
          continue;
        if (!nonleaf->test(u1 >> UPART_BITS))
          continue;
        node_t u = cuckoo[us[0] = u0];
        node_t v0 = sipnode(&ctx->sip_ctx, nonce, 1);
        u32 nu, nv;
        if (u != 0 && (us[nu = 1] = u) == (vs[nv = 0] = v0)) {
          printf(" 2-cycle found at %d:%d\n", tp->id, depth);
          solution(ctx, us, nu, vs, nv);
          pthread_exit(NULL);
        }
        cuckoo.set(u0, v0);
      }
      barrier(&ctx->barry);
      if (tp->id == 0 && cuckoo.load() >= 90) {
        printf("OVERLOAD !!!!!!!!!!!!!!!!!\n");
        break;
      }
    }
    if (tp->id == 0) {
      printf("upart %d depth %d load %d%%\n", upart, PROOFSIZE/2, cuckoo.load());
      cuckoo.clear();
      ctx->nonleaf->reset();
    }
  }
  pthread_exit(NULL);
}
