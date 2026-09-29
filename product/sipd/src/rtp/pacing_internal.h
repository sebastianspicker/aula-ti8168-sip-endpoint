#ifndef AULA_RTP_PACING_INTERNAL_H
#define AULA_RTP_PACING_INTERNAL_H

#include "aula_sipd/rtp.h"

typedef struct aula_rtp_send_batch aula_rtp_send_batch;

aula_status aula_rtp_send_queue_batch_begin(
    aula_rtp_send_queue *queue, uint32_t packet_count, uint32_t payload_bytes,
    uint64_t now_ns, aula_rtp_send_batch **out_batch);
aula_status aula_rtp_send_batch_stage(aula_rtp_send_batch *batch,
                                        const aula_rtp_packet *packet);
aula_status aula_rtp_send_batch_commit(aula_rtp_send_batch *batch);
void aula_rtp_send_batch_abort(aula_rtp_send_batch *batch);

/* Preserve cadence across scheduling jitter, with at most max_burst due
 * packets at one instant. The public dequeue API retains strict spacing. */
aula_status aula_rtp_send_queue_dequeue_bounded(aula_rtp_send_queue *queue,
    uint64_t now_ns, uint32_t max_burst, aula_rtp_packet *out_packet);

#if defined(AULA_SIPD_TEST_FAULTS) && AULA_SIPD_TEST_FAULTS
void aula_rtp_send_queue_test_fail_allocation_after(int allocation_count);
#endif

#endif
