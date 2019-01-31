/*
 * ngtcp2
 *
 * Copyright (c) 2019 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef NGTCP2_PV_H
#define NGTCP2_PV_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <ngtcp2/ngtcp2.h>

#include "ngtcp2_ringbuf.h"

struct ngtcp2_log;
typedef struct ngtcp2_log ngtcp2_log;

typedef struct {
  /* expiry is the timestamp when this PATH_CHALLENGE expires. */
  ngtcp2_tstamp expiry;
  uint8_t data[8];
} ngtcp2_pv_entry;

void ngtcp2_pv_entry_init(ngtcp2_pv_entry *pvent, const uint8_t *data,
                          ngtcp2_tstamp expiry);

typedef enum {
  NGTCP2_PV_FLAG_NONE,
  NGTCP2_PV_FLAG_BLOCKING = 0x01,
  /* NGTCP2_PV_FLAG_DONT_CARE indicates that the outcome of the path
     validation does not matter. */
  NGTCP2_PV_FLAG_DONT_CARE = 0x02,
  /* NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH indicates that DCID should
     be retired after path validation finishes regardless of its
     result. */
  NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH = 0x04,
} ngtcp2_pv_flag;

struct ngtcp2_pv;
typedef struct ngtcp2_pv ngtcp2_pv;

/* TODO It might be nice to express that path validation is blocking
   or not. */
struct ngtcp2_pv {
  ngtcp2_mem *mem;
  ngtcp2_log *log;
  ngtcp2_pv *next;
  /* path is the network path which this validator validates. */
  ngtcp2_path path;
  ngtcp2_cid dcid;
  /* ents is the ring buffer of ngtcp2_pv_entry */
  ngtcp2_ringbuf ents;
  /* timeout is the duration within which this path validation should
     succeed. */
  ngtcp2_duration timeout;
  ngtcp2_tstamp started_ts;
  /* seq is the sequence number of dcid */
  uint64_t seq;
  /* loss_count is the number of lost PATH_CHALLENGE */
  size_t loss_count;
  uint8_t flags;
};

int ngtcp2_pv_new(ngtcp2_pv **ppv, const ngtcp2_path *path,
                  const ngtcp2_cid *dcid, ngtcp2_duration timeout,
                  uint8_t flags, ngtcp2_log *log, ngtcp2_mem *mem);

void ngtcp2_pv_del(ngtcp2_pv *pv);

void ngtcp2_pv_ensure_start(ngtcp2_pv *pv, ngtcp2_tstamp ts);

/*
 * ngtcp2_pv_add_entry adds new entry with |data|.  |expiry| is the
 * expiry time of the entry.
 */
void ngtcp2_pv_add_entry(ngtcp2_pv *pv, const uint8_t *data,
                         ngtcp2_tstamp expiry);

/*
 * ngtcp2_pv_full returns nonzero if |pv| is full of ngtcp2_pv_entry.
 */
int ngtcp2_pv_full(ngtcp2_pv *pv);

/*
 * ngtcp2_pv_verify verifies that the |data| received from |path|
 * matches the one of the existing entry.
 */
int ngtcp2_pv_verify(ngtcp2_pv *pv, const ngtcp2_path *path,
                     const uint8_t *data);

/*
 * ngtcp2_pv_handle_entry_expiry checks expiry for each entry.
 */
void ngtcp2_pv_handle_entry_expiry(ngtcp2_pv *pv, ngtcp2_tstamp ts);

/*
 * ngtcp2_pv_validation_timed_out returns nonzero if the path
 * validation fails because of timeout.
 */
int ngtcp2_pv_validation_timed_out(ngtcp2_pv *pv, ngtcp2_tstamp ts);

ngtcp2_tstamp ngtcp2_pv_next_expiry(ngtcp2_pv *pv);

int ngtcp2_pv_on_finish(ngtcp2_pv *pv, ngtcp2_frame_chain **pfrc);

#endif /* NGTCP2_PV_H */
