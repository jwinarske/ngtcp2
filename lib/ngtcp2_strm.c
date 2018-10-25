/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
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
#include "ngtcp2_strm.h"

#include <string.h>
#include <assert.h>

#include "ngtcp2_rtb.h"
#include "ngtcp2_pkt.h"
#include "ngtcp2_str.h"

static int offset_less(int64_t lhs, int64_t rhs) { return lhs < rhs; }

int ngtcp2_strm_init(ngtcp2_strm *strm, uint64_t stream_id, uint32_t flags,
                     uint64_t max_rx_offset, uint64_t max_tx_offset,
                     void *stream_user_data, ngtcp2_mem *mem) {
  int rv;

  strm->cycle = 0;
  strm->tx_offset = 0;
  strm->last_rx_offset = 0;
  strm->nbuffered = 0;
  strm->stream_id = stream_id;
  strm->flags = flags;
  strm->stream_user_data = stream_user_data;
  strm->max_rx_offset = strm->unsent_max_rx_offset = max_rx_offset;
  strm->max_tx_offset = max_tx_offset;
  strm->me.key = stream_id;
  strm->me.next = NULL;
  strm->pe.index = SIZE_MAX;
  strm->mem = mem;
  /* Initializing to 0 is a bit controversial because application
     error code 0 is STOPPING.  But STOPPING is only sent with
     RST_STREAM in response to STOP_SENDING, and it is not used to
     indicate the cause of closure.  So effectively, 0 means "no
     error." */
  strm->app_error_code = 0;
  memset(&strm->tx_buf, 0, sizeof(strm->tx_buf));

  rv = ngtcp2_gaptr_init(&strm->acked_tx_offset, mem);
  if (rv != 0) {
    goto fail_gaptr_init;
  }

  rv = ngtcp2_rob_init(&strm->rob, 8 * 1024, mem);
  if (rv != 0) {
    goto fail_rob_init;
  }

  rv = ngtcp2_ksl_init(&strm->streamfrq, offset_less, INT64_MAX, mem);
  if (rv != 0) {
    goto fail_ksl_init;
  }

  return 0;

fail_ksl_init:
  ngtcp2_rob_free(&strm->rob);
fail_rob_init:
  ngtcp2_gaptr_free(&strm->acked_tx_offset);
fail_gaptr_init:
  return rv;
}

void ngtcp2_strm_free(ngtcp2_strm *strm) {
  ngtcp2_ksl_it it;

  if (strm == NULL) {
    return;
  }

  for (it = ngtcp2_ksl_begin(&strm->streamfrq); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    ngtcp2_frame_chain_del(ngtcp2_ksl_it_get(&it), strm->mem);
  }

  ngtcp2_ksl_free(&strm->streamfrq);
  ngtcp2_rob_free(&strm->rob);
  ngtcp2_gaptr_free(&strm->acked_tx_offset);
}

uint64_t ngtcp2_strm_rx_offset(ngtcp2_strm *strm) {
  return ngtcp2_rob_first_gap_offset(&strm->rob);
}

int ngtcp2_strm_recv_reordering(ngtcp2_strm *strm, const uint8_t *data,
                                size_t datalen, uint64_t offset) {
  return ngtcp2_rob_push(&strm->rob, offset, data, datalen);
}

void ngtcp2_strm_shutdown(ngtcp2_strm *strm, uint32_t flags) {
  strm->flags |= flags & NGTCP2_STRM_FLAG_SHUT_RDWR;
}

int ngtcp2_strm_push_stream_frame(ngtcp2_strm *strm, ngtcp2_frame_chain *frc) {
  ngtcp2_frame *fr = &frc->fr;

  assert(fr->type == NGTCP2_FRAME_STREAM);
  assert(frc->next == NULL);

  return ngtcp2_ksl_insert(&strm->streamfrq, NULL, (int64_t)fr->stream.offset,
                           frc);
}

int ngtcp2_strm_pop_stream_frame(ngtcp2_strm *strm, ngtcp2_frame_chain **pfrc,
                                 size_t left) {
  ngtcp2_stream *fr, *nfr;
  ngtcp2_frame_chain *frc, *nfrc;
  ngtcp2_ksl_it it = ngtcp2_ksl_begin(&strm->streamfrq);
  int rv;
  size_t nmerged;
  size_t datalen;

  if (ngtcp2_ksl_it_end(&it)) {
    *pfrc = NULL;
    return 0;
  }

  frc = ngtcp2_ksl_it_get(&it);
  rv =
      ngtcp2_ksl_remove(&strm->streamfrq, NULL, (int64_t)frc->fr.stream.offset);
  if (rv != 0) {
    return rv;
  }

  fr = &frc->fr.stream;

  if (fr->datacnt == NGTCP2_MAX_STREAM_DATACNT) {
    *pfrc = frc;
    return 0;
  }

  datalen = ngtcp2_vec_len(fr->data, fr->datacnt);
  if (datalen > left) {
    rv = ngtcp2_frame_chain_extralen_new(
        &nfrc, sizeof(ngtcp2_vec) * (NGTCP2_MAX_STREAM_DATACNT - 1), strm->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_frame_chain_del(frc, strm->mem);
      return rv;
    }

    rv = ngtcp2_ksl_insert(&strm->streamfrq, NULL, (int64_t)(fr->offset + left),
                           nfrc);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_frame_chain_del(nfrc, strm->mem);
      ngtcp2_frame_chain_del(frc, strm->mem);
      return rv;
    }

    nfr = &nfrc->fr.stream;
    nfr->type = NGTCP2_FRAME_STREAM;
    nfr->flags = 0;
    nfr->fin = fr->fin;
    nfr->stream_id = fr->stream_id;
    nfr->offset = fr->offset + left;
    nfr->datacnt = 0;

    ngtcp2_vec_split(fr->data, &fr->datacnt, nfr->data, &nfr->datacnt, left);

    fr->fin = 0;

    *pfrc = frc;

    return 0;
  }

  left -= datalen;

  for (; !ngtcp2_ksl_it_end(&it);) {
    nfrc = ngtcp2_ksl_it_get(&it);
    nfr = &nfrc->fr.stream;

    if (nfr->offset != fr->offset + datalen) {
      assert(fr->offset + datalen < nfr->offset);
      break;
    }

    nmerged = ngtcp2_vec_merge(fr->data, &fr->datacnt, nfr->data, &nfr->datacnt,
                               left, NGTCP2_MAX_STREAM_DATACNT);
    if (nmerged == 0) {
      break;
    }

    rv = ngtcp2_ksl_remove(&strm->streamfrq, NULL, (int64_t)nfr->offset);
    if (rv != 0) {
      ngtcp2_frame_chain_del(frc, strm->mem);
      return rv;
    }

    datalen += nmerged;
    nfr->offset += nmerged;
    left -= nmerged;

    if (nfr->datacnt == 0) {
      ngtcp2_frame_chain_del(nfrc, strm->mem);
    } else {
      rv =
          ngtcp2_ksl_insert(&strm->streamfrq, NULL, (int64_t)nfr->offset, nfrc);
      if (rv != 0) {
        ngtcp2_frame_chain_del(nfrc, strm->mem);
        ngtcp2_frame_chain_del(frc, strm->mem);
        return rv;
      }
    }

    if (fr->datacnt == NGTCP2_MAX_STREAM_DATACNT || left == 0) {
      break;
    }
  }

  *pfrc = frc;
  return 0;
}

int ngtcp2_strm_stream_frame_empty(ngtcp2_strm *strm) {
  return ngtcp2_ksl_len(&strm->streamfrq) == 0;
}

void ngtcp2_strm_clear_stream_frame(ngtcp2_strm *strm) {
  ngtcp2_ksl_it it;

  for (it = ngtcp2_ksl_begin(&strm->streamfrq); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    ngtcp2_frame_chain_del(ngtcp2_ksl_it_get(&it), strm->mem);
  }

  ngtcp2_ksl_clear(&strm->streamfrq);
}

int ngtcp2_strm_is_tx_queued(ngtcp2_strm *strm) {
  return strm->pe.index != SIZE_MAX;
}
