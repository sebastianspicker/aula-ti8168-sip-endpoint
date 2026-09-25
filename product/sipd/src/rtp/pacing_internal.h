#ifndef LS200_RTP_PACING_INTERNAL_H
#define LS200_RTP_PACING_INTERNAL_H

#include "ls200_sipd/rtp.h"

typedef struct ls200_rtp_send_batch ls200_rtp_send_batch;

ls200_status ls200_rtp_send_queue_batch_begin(
    ls200_rtp_send_queue *queue, uint32_t packet_count, uint32_t payload_bytes,
    uint64_t now_ns, ls200_rtp_send_batch **out_batch);
ls200_status ls200_rtp_send_batch_stage(ls200_rtp_send_batch *batch,
                                        const ls200_rtp_packet *packet);
ls200_status ls200_rtp_send_batch_commit(ls200_rtp_send_batch *batch);
void ls200_rtp_send_batch_abort(ls200_rtp_send_batch *batch);

/* Preserve cadence across scheduling jitter, with at most max_burst due
 * packets at one instant. The public dequeue API retains strict spacing. */
ls200_status ls200_rtp_send_queue_dequeue_bounded(ls200_rtp_send_queue *queue,
    uint64_t now_ns, uint32_t max_burst, ls200_rtp_packet *out_packet);

#if defined(LS200_SIPD_TEST_FAULTS) && LS200_SIPD_TEST_FAULTS
void ls200_rtp_send_queue_test_fail_allocation_after(int allocation_count);
#endif

#endif
