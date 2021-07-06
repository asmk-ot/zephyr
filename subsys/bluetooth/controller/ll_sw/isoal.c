/*
 * Copyright (c) 2021 Demant
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/types.h>
#include <sys/types.h>
#include <toolchain.h>
#include <sys/util.h>

#include "util/memq.h"
#include "pdu.h"
#include "lll.h"
#include "isoal.h"

#define LOG_MODULE_NAME bt_ctlr_isoal
#include "common/log.h"
#include "hal/debug.h"

/* TODO this must be taken from a Kconfig */
#define ISOAL_SINKS_MAX   (4)

/** Allocation state */
typedef uint8_t isoal_alloc_state_t;
#define ISOAL_ALLOC_STATE_FREE            ((isoal_alloc_state_t) 0x00)
#define ISOAL_ALLOC_STATE_TAKEN           ((isoal_alloc_state_t) 0x01)

static struct
{
	isoal_alloc_state_t  sink_allocated[ISOAL_SINKS_MAX];
	struct isoal_sink    sink_state[ISOAL_SINKS_MAX];
} isoal_global;


/**
 * @brief Internal reset
 * Zero-init entire ISO-AL state
 */
static isoal_status_t isoal_init_reset(void)
{
	memset(&isoal_global, 0, sizeof(isoal_global));
	return ISOAL_STATUS_OK;
}

/**
 * @brief  Initialize ISO-AL
 */
isoal_status_t isoal_init(void)
{
	isoal_status_t err = ISOAL_STATUS_OK;

	err = isoal_init_reset();
	if (err) {
		return err;
	}

	return err;
}

/** Clean up and reinitialize */
isoal_status_t isoal_reset(void)
{
	isoal_status_t err = ISOAL_STATUS_OK;

	err = isoal_init_reset();
	if (err) {
		return err;
	}

	return err;
}

/**
 * @brief Find free sink from statically-sized pool and allocate it
 * @details Implemented as linear search since pool is very small
 *
 * @param hdl[out]  Handle to sink
 * @return ISOAL_STATUS_OK if we could allocate; otherwise ISOAL_STATUS_ERR_SINK_ALLOC
 */
static isoal_status_t isoal_sink_allocate(isoal_sink_handle_t *hdl)
{
	isoal_sink_handle_t i;

	/* Very small linear search to find first free */
	for (i = 0; i < ISOAL_SINKS_MAX; i++) {
		if (isoal_global.sink_allocated[i] == ISOAL_ALLOC_STATE_FREE) {
			isoal_global.sink_allocated[i] = ISOAL_ALLOC_STATE_TAKEN;
			*hdl = i;
			return ISOAL_STATUS_OK;
		}
	}

	return ISOAL_STATUS_ERR_SINK_ALLOC; /* All entries were taken */
}

/**
 * @brief Mark a sink as being free to allocate again
 * @param hdl[in]  Handle to sink
 */
static void isoal_sink_deallocate(isoal_sink_handle_t hdl)
{
	isoal_global.sink_allocated[hdl] = ISOAL_ALLOC_STATE_FREE;
}

/**
 * @brief Create a new sink
 *
 * @param hdl[out]           Handle to new sink
 * @param handle[in]         Connection handle
 * @param role[in]           Peripheral or Central
 * @param burst_number[in]   Burst Number
 * @param flush_timeout[in]  Flush timeout
 * @param sdu_interval[in]   SDU interval
 * @param iso_interval[in]   ISO interval
 * @param cis_sync_delay[in] CIS sync delay
 * @param cig_sync_delay[in] CIG sync delay
 * @param sdu_alloc[in]      Callback of SDU allocator
 * @param sdu_emit[in]       Callback of SDU emitter
 * @param sdu_write[in]      Callback of SDU byte writer
 * @return ISOAL_STATUS_OK if we could create a new sink; otherwise ISOAL_STATUS_ERR_SINK_ALLOC
 */
isoal_status_t isoal_sink_create(
	isoal_sink_handle_t      *hdl,
	uint16_t                 handle,
	uint8_t                  role,
	uint8_t                  burst_number,
	uint8_t                  flush_timeout,
	uint32_t                 sdu_interval,
	uint16_t                 iso_interval,
	uint32_t                 cis_sync_delay,
	uint32_t                 cig_sync_delay,
	isoal_sink_sdu_alloc_cb  sdu_alloc,
	isoal_sink_sdu_emit_cb   sdu_emit,
	isoal_sink_sdu_write_cb  sdu_write)
{
	isoal_status_t err;

	/* Allocate a new sink */
	err = isoal_sink_allocate(hdl);
	if (err) {
		return err;
	}

	isoal_global.sink_state[*hdl].session.handle = handle;

	/* Todo: Next section computing various constants, should potentially be a
	 * function in itself as a number of the dependencies could be changed while
	 * a connection is active.
	 */

	/* Note: sdu_interval unit is uS, iso_interval is a multiple of 1.25mS */
	isoal_global.sink_state[*hdl].session.pdus_per_sdu =
		burst_number * (sdu_interval / (iso_interval * 1250));

	/* Computation of transport latency (constant part)
	 *
	 * Unframed case:
	 *
	 * M->S: SDU_Synchronization_Reference =
	 *   CIS reference anchor point + CIS_Sync_Delay + (FT_M_To_S - 1) * ISO_Interval
	 *
	 * S->M: SDU_Synchronization_Reference =
	 *   CIS reference anchor point + CIS_Sync_Delay - CIG_Sync_Delay -
	 *   ((ISO_Interval / SDU interval)-1) * SDU interval
	 *
	 * Framed case:
	 *
	 * M->S: SDU_Synchronization_Reference =
	 *   CIS Reference Anchor point +
	 *   CIS_Sync_Delay + SDU_Interval_M_To_S + FT_M_To_S * ISO_Interval -
	 *   Time_Offset
	 *
	 * S->M: synchronization reference SDU = CIS reference anchor point +
	 *   CIS_Sync_Delay - CIG_Sync_Delay - Time_Offset
	 */
	if (role == BT_CONN_ROLE_SLAVE) {
		isoal_global.sink_state[*hdl].session.latency_unframed =
			cis_sync_delay + ((flush_timeout - 1) * iso_interval);

		isoal_global.sink_state[*hdl].session.latency_framed =
			cis_sync_delay + sdu_interval + (flush_timeout * iso_interval);
	} else {
		isoal_global.sink_state[*hdl].session.latency_unframed =
			cis_sync_delay - cig_sync_delay -
			(((iso_interval / sdu_interval) - 1) * iso_interval);

		isoal_global.sink_state[*hdl].session.latency_framed =
			cis_sync_delay - cig_sync_delay;
	}

	/* Remember the platform-specific callbacks */
	isoal_global.sink_state[*hdl].session.sdu_alloc = sdu_alloc;
	isoal_global.sink_state[*hdl].session.sdu_emit  = sdu_emit;
	isoal_global.sink_state[*hdl].session.sdu_write = sdu_write;

	/* Initialize running seq number to zero */
	isoal_global.sink_state[*hdl].session.seqn = 0;

	return err;
}

/**
 * @brief Get reference to configuration struct
 *
 * @param hdl[in]   Handle to new sink
 * @return Reference to parameter struct, to be configured by caller
 */
struct isoal_sink_config *isoal_get_sink_param_ref(isoal_sink_handle_t hdl)
{
	LL_ASSERT(isoal_global.sink_allocated[hdl] == ISOAL_ALLOC_STATE_TAKEN);

	return &isoal_global.sink_state[hdl].session.param;
}

/**
 * @brief Atomically enable latch-in of packets and SDU production
 * @param hdl[in]  Handle of existing instance
 */
void isoal_sink_enable(isoal_sink_handle_t hdl)
{
	/* Reset bookkeeping state */
	memset(&isoal_global.sink_state[hdl].sdu_production, 0,
	       sizeof(isoal_global.sink_state[hdl].sdu_production));

	/* Atomically enable */
	isoal_global.sink_state[hdl].sdu_production.mode = ISOAL_PRODUCTION_MODE_ENABLED;
}

/**
 * @brief Atomically disable latch-in of packets and SDU production
 * @param hdl[in]  Handle of existing instance
 */
void isoal_sink_disable(isoal_sink_handle_t hdl)
{
	/* Atomically disable */
	isoal_global.sink_state[hdl].sdu_production.mode = ISOAL_PRODUCTION_MODE_DISABLED;
}

/**
 * @brief Disable and deallocate existing sink
 * @param hdl[in]  Handle of existing instance
 */
void isoal_sink_destroy(isoal_sink_handle_t hdl)
{
	/* Atomic disable */
	isoal_sink_disable(hdl);

	/* Permit allocation anew */
	isoal_sink_deallocate(hdl);
}

/* Obtain destination SDU */
static isoal_status_t isoal_rx_allocate_sdu(struct isoal_sink *sink,
					    const struct isoal_pdu_rx *pdu_meta)
{
	isoal_status_t err = ISOAL_STATUS_OK;
	struct isoal_sdu_produced *sdu = &sink->sdu_production.sdu;

	/* Allocate a SDU if the previous was filled (thus sent) */
	const bool sdu_complete = (sink->sdu_production.sdu_available == 0);

	if (sdu_complete) {
		/* Allocate new clean SDU buffer */
		err = sink->session.sdu_alloc(
			sink,
			pdu_meta,      /* [in]  PDU origin may determine buffer */
			&sdu->contents  /* [out] Updated with pointer and size */
		);

		/* Nothing has been written into buffer yet */
		sink->sdu_production.sdu_written   = 0;
		sink->sdu_production.sdu_available = sdu->contents.size;
		LL_ASSERT(sdu->contents.size > 0);

		/* Remember meta data */
		sdu->status = pdu_meta->meta->status;
		sdu->timestamp = pdu_meta->meta->timestamp;
		/* Get seq number from session counter */
		sdu->seqn = sink->session.seqn;
	}

	return err;
}

static isoal_status_t isoal_rx_try_emit_sdu(struct isoal_sink *sink, bool end_of_sdu)
{
	isoal_status_t err = ISOAL_STATUS_OK;
	struct isoal_sdu_produced *sdu = &sink->sdu_production.sdu;

	/* Emit a SDU */
	const bool sdu_complete = (sink->sdu_production.sdu_available == 0) || end_of_sdu;

	if (end_of_sdu) {
		sink->sdu_production.sdu_available = 0;
	}

	if (sdu_complete) {
		uint8_t next_state = BT_ISO_START;

		switch (sink->sdu_production.sdu_state) {
		case BT_ISO_START:
			if (end_of_sdu) {
				sink->sdu_production.sdu_state = BT_ISO_SINGLE;
				next_state = BT_ISO_START;
			} else {
				sink->sdu_production.sdu_state = BT_ISO_START;
				next_state = BT_ISO_CONT;
			}
			break;
		case BT_ISO_CONT:
			if (end_of_sdu) {
				sink->sdu_production.sdu_state  = BT_ISO_END;
				next_state = BT_ISO_START;
			} else {
				sink->sdu_production.sdu_state  = BT_ISO_CONT;
				next_state = BT_ISO_CONT;
			}
			break;
		case BT_ISO_END:
		case BT_ISO_SINGLE:
		default:
			LL_ASSERT(0);
			break;
		}
		sdu->status = sink->sdu_production.sdu_status;
		err = sink->session.sdu_emit(sink, sdu);

		/* update next state */
		sink->sdu_production.sdu_state = next_state;
	}

	return err;
}

static isoal_status_t isoal_rx_append_to_sdu(struct isoal_sink *sink,
					     const struct isoal_pdu_rx *pdu_meta,
					     uint8_t offset,
					     uint8_t length,
					     bool is_end_fragment)
{
	isoal_status_t err = ISOAL_STATUS_OK;
	bool handle_error_case;
	const uint8_t *pdu_payload          = pdu_meta->pdu->cis.payload + offset;
	isoal_pdu_len_t packet_available    = length;

	/* Might get an empty packed due to errors, we will need to terminate
	 * and send something up anyhow
	 */
	handle_error_case = (is_end_fragment && (packet_available == 0));

	LL_ASSERT(pdu_payload);

	/* While there is something left of the packet to consume */
	while ((packet_available > 0) || handle_error_case) {
		const isoal_status_t err_alloc = isoal_rx_allocate_sdu(sink, pdu_meta);
		struct isoal_sdu_produced *sdu = &sink->sdu_production.sdu;

		err |= err_alloc;

		/*
		 * For this SDU we can only consume of packet, bounded by:
		 *   - What can fit in the destination SDU.
		 *   - What remains of the packet.
		 */
		const size_t consume_len = MIN(
			packet_available,
			sink->sdu_production.sdu_available
		);

		LL_ASSERT(sdu->contents.dbuf);

		if (consume_len > 0) {
			if (pdu_meta->meta->status == ISOAL_PDU_STATUS_VALID) {
				err |= sink->session.sdu_write(sdu->contents.dbuf,
							       pdu_payload,
							       consume_len);
			}
			pdu_payload += consume_len;
			sink->sdu_production.sdu_written   += consume_len;
			sink->sdu_production.sdu_available -= consume_len;
			packet_available                   -= consume_len;
		}
		bool end_of_sdu = (packet_available == 0) && is_end_fragment;

		const isoal_status_t err_emit = isoal_rx_try_emit_sdu(sink, end_of_sdu);

		handle_error_case = false;
		err |= err_emit;
	}

	LL_ASSERT(packet_available == 0);
	return err;
}


/**
 * @brief Consume an unframed PDU: Copy contents into SDU(s) and emit to a sink
 * @details Destination sink may have an already partially built SDU
 *
 * @param sink[in,out]    Destination sink with bookkeeping state
 * @param pdu_meta[out]  PDU with meta information (origin, timing, status)
 *
 * @return Status
 */
static isoal_status_t isoal_rx_unframed_consume(struct isoal_sink *sink,
						const struct isoal_pdu_rx *pdu_meta)
{
	isoal_status_t err;
	uint8_t next_state, llid;
	bool pdu_err, pdu_padding, last_pdu, end_of_packet, seq_err;

	err = ISOAL_STATUS_OK;
	next_state = ISOAL_START;

	llid = pdu_meta->pdu->cis.ll_id;
	pdu_err = (pdu_meta->meta->status != ISOAL_PDU_STATUS_VALID);
	pdu_padding = (pdu_meta->pdu->cis.length == 0) && (llid == PDU_BIS_LLID_START_CONTINUE);

	if (sink->sdu_production.fsm == ISOAL_START) {
		sink->sdu_production.sdu_status = ISOAL_SDU_STATUS_VALID;
		sink->sdu_production.sdu_state = BT_ISO_START;
		sink->sdu_production.pdu_cnt = 1;
		sink->session.seqn++;
		seq_err = false;

		/* Todo: anchorpoint must be reference anchor point, should be fixed in LL */
		uint32_t anchorpoint = pdu_meta->meta->timestamp;
		uint32_t latency = sink->session.latency_unframed;

		sink->sdu_production.sdu.timestamp = anchorpoint + latency;
	} else {
		sink->sdu_production.pdu_cnt++;
		seq_err = (pdu_meta->meta->payload_number != sink->sdu_production.prev_pdu_id+1);
	}

	last_pdu = (sink->sdu_production.pdu_cnt == sink->session.pdus_per_sdu);
	end_of_packet = (llid == PDU_BIS_LLID_COMPLETE_END) || last_pdu;

	switch (sink->sdu_production.fsm) {

	case ISOAL_START:
	case ISOAL_CONTINUE:
		if (pdu_err || seq_err) {
			/* PDU contains errors */
			next_state = ISOAL_ERR_SPOOL;
		} else if (llid == PDU_BIS_LLID_START_CONTINUE) {
			/* PDU contains a continuation (neither start of end) fragment of SDU */
			if (last_pdu) {
				/* last pdu in sdu, but end fragment not seen, emit with error */
				next_state = ISOAL_START;
			} else {
				next_state = ISOAL_CONTINUE;
			}
		} else if (llid == PDU_BIS_LLID_COMPLETE_END) {
			/* PDU contains end fragment of a fragmented SDU */
			if (last_pdu) {
				/* Last PDU all done */
				next_state = ISOAL_START;
			} else {
				/* Padding after end fragment to follow */
				next_state = ISOAL_ERR_SPOOL;
			}
		} else  {
			/* Unsupported case */
			LL_ASSERT(0);
		}
		break;

	case ISOAL_ERR_SPOOL:
		/* State assumes that at end fragment or err has been seen,
		 * now just consume the rest
		 */
		if (last_pdu) {
			/* Last padding seen, restart */
			next_state = ISOAL_START;
		} else {
			next_state = ISOAL_ERR_SPOOL;
		}
		break;

	}

	/* Update error state */
	if (pdu_err && !pdu_padding) {
		sink->sdu_production.sdu_status |= pdu_meta->meta->status;
	} else if (last_pdu && (llid != PDU_BIS_LLID_COMPLETE_END) &&
				(sink->sdu_production.fsm  != ISOAL_ERR_SPOOL)) {
		/* END fragment never seen */
		sink->sdu_production.sdu_status |= ISOAL_SDU_STATUS_ERRORS;
	} else if (seq_err) {
		sink->sdu_production.sdu_status |= ISOAL_SDU_STATUS_LOST_DATA;
	}

	/* Append valid PDU to SDU */
	if (!pdu_padding && !pdu_err) {
		err |= isoal_rx_append_to_sdu(sink, pdu_meta, 0,
					      pdu_meta->pdu->cis.length,
					      end_of_packet);
	}

	/* Update next state */
	sink->sdu_production.fsm = next_state;
	sink->sdu_production.prev_pdu_id = pdu_meta->meta->payload_number;

	return err;
}


/**
 * @brief Consume a framed PDU: Copy contents into SDU(s) and emit to a sink
 * @details Destination sink may have an already partially built SDU
 *
 * @param sink[in,out]   Destination sink with bookkeeping state
 * @param pdu_meta[out]  PDU with meta information (origin, timing, status)
 *
 * @return Status
 */
static isoal_status_t isoal_rx_framed_consume(struct isoal_sink *sink,
					      const struct isoal_pdu_rx *pdu_meta)
{
	struct pdu_iso_sdu_sh *seg_hdr;
	isoal_status_t err;
	uint8_t next_state;
	uint8_t *end_of_pdu;
	bool pdu_err, seq_err, pdu_padding;

	err = ISOAL_STATUS_OK;
	next_state = ISOAL_START;
	pdu_err = (pdu_meta->meta->status != ISOAL_PDU_STATUS_VALID);
	pdu_padding = (pdu_meta->pdu->cis.length == 0);

	if (sink->sdu_production.fsm == ISOAL_START) {
		seq_err = false;
	} else {
		seq_err = (pdu_meta->meta->payload_number != sink->sdu_production.prev_pdu_id+1);
	}

	end_of_pdu = ((uint8_t *) pdu_meta->pdu->cis.payload) + pdu_meta->pdu->cis.length - 1;
	seg_hdr = (struct pdu_iso_sdu_sh *) pdu_meta->pdu->cis.payload;

	if (pdu_err || seq_err) {
		/* When one or more ISO Data PDUs are not received, the receiving device may
		 * discard all SDUs affected by the missing PDUs. Any partially received SDU
		 * may also be discarded.
		 */
		next_state = ISOAL_ERR_SPOOL;

		if (pdu_err) {
			sink->sdu_production.sdu_status |= pdu_meta->meta->status;
		} else if (seq_err) {
			sink->sdu_production.sdu_status |= ISOAL_SDU_STATUS_LOST_DATA;
		}

		/* Flush current SDU with error if any */
		err |= isoal_rx_append_to_sdu(sink, pdu_meta, 0, 0, true);

		/* Skip searching this PDU */
		seg_hdr = NULL;
	}

	if (pdu_padding) {
		/* Skip searching this PDU */
		seg_hdr = NULL;
	}

	while (seg_hdr) {

		bool append = true;
		const uint8_t sc    = seg_hdr->sc;
		const uint8_t cmplt = seg_hdr->cmplt;

		if (sink->sdu_production.fsm == ISOAL_START) {
			sink->sdu_production.sdu_status = ISOAL_SDU_STATUS_VALID;
			sink->sdu_production.sdu_state  = BT_ISO_START;
			sink->session.seqn++;
		}

		switch (sink->sdu_production.fsm) {

		case ISOAL_START: {
			uint32_t timeoffset = seg_hdr->timeoffset;
			uint32_t anchorpoint = pdu_meta->meta->timestamp;
			uint32_t latency = sink->session.latency_framed;
			uint32_t timestamp = anchorpoint + latency - timeoffset;

			if (!sc && !cmplt) {
				/* The start of a new SDU, where not all SDU data is included in
				 * the current PDU, and additional PDUs are required to complete
				 * the SDU.
				 */
				sink->sdu_production.sdu.timestamp = timestamp;
				next_state = ISOAL_CONTINUE;
			} else if (!sc && cmplt) {
				/* The start of a new SDU that contains the full SDU data in the
				 * current PDU.
				 */
				sink->sdu_production.sdu.timestamp = timestamp;
				next_state = ISOAL_START;
			} else {
				/* Unsupported case */
				LL_ASSERT(0);
			}
			break;
		}
		case ISOAL_CONTINUE: {
			if (sc && !cmplt) {
				/* The continuation of a previous SDU. The SDU payload is appended
				 * to the previous data and additional PDUs are required to
				 * complete the SDU.
				 */
				next_state = ISOAL_CONTINUE;
			} else if (sc && cmplt) {
				/* The continuation of a previous SDU.
				 * Frame data is appended to previously received SDU data and
				 * completes in the current PDU.
				 */
				next_state = ISOAL_START;
			} else {
				/* Unsupported case */
				LL_ASSERT(0);
			}
			break;
		}
		case ISOAL_ERR_SPOOL: {
			/* In error state, search for valid next start of SDU */
			uint32_t timeoffset = seg_hdr->timeoffset;
			uint32_t anchorpoint = pdu_meta->meta->timestamp;
			uint32_t latency = sink->session.latency_framed;
			uint32_t timestamp = anchorpoint + latency - timeoffset;

			if (!sc && !cmplt) {
				/* The start of a new SDU, where not all SDU data is included in
				 * the current PDU, and additional PDUs are required to complete
				 * the SDU.
				 */
				sink->sdu_production.sdu.timestamp = timestamp;
				next_state = ISOAL_CONTINUE;
			} else if (!sc && cmplt) {
				/* The start of a new SDU that contains the full SDU data in the
				 * current PDU.
				 */
				sink->sdu_production.sdu.timestamp = timestamp;
				next_state = ISOAL_START;
			} else {
				/* Start not found yet, stay in Error state */
				append = false;
				next_state = ISOAL_ERR_SPOOL;
			}
			break;
		}

		}

		if (append) {
			/* Calculate offset of first payload byte from SDU based on assumption
			 * of No time_offset in header
			 */
			uint8_t offset = ((uint8_t *) seg_hdr) + PDU_ISO_SEG_HDR_SIZE -
					 pdu_meta->pdu->cis.payload;
			uint8_t length = seg_hdr->length;

			if (!sc) {
				/* time_offset included in header, don't copy offset field to SDU */
				offset = offset + PDU_ISO_SEG_TIMEOFFSET_SIZE;
				length = length - PDU_ISO_SEG_TIMEOFFSET_SIZE;
			}

			/* Todo: check if effective len=0 what happens then?
			 * We should possibly be able to send empty packets with only time stamp
			 */

			err |= isoal_rx_append_to_sdu(sink, pdu_meta, offset, length, cmplt);
		}

		/* Update next state */
		sink->sdu_production.fsm = next_state;

		/* Find next segment header, set to null if past end of PDU */
		seg_hdr = (struct pdu_iso_sdu_sh *) (((uint8_t *) seg_hdr) +
						     seg_hdr->length + PDU_ISO_SEG_HDR_SIZE);

		if (((uint8_t *) seg_hdr) > end_of_pdu) {
			seg_hdr = NULL;
		}
	}

	sink->sdu_production.prev_pdu_id = pdu_meta->meta->payload_number;

	return err;
}

/**
 * @brief Deep copy a PDU, recombine into SDU(s)
 * @details Recombination will occur individually for every enabled sink
 *
 * @param sink_hdl[in] Handle of destination sink
 * @param pdu_meta[in] PDU along with meta information (origin, timing, status)
 * @return Status
 */
isoal_status_t isoal_rx_pdu_recombine(isoal_sink_handle_t sink_hdl,
				      const struct isoal_pdu_rx *pdu_meta)
{
	struct isoal_sink *sink = &isoal_global.sink_state[sink_hdl];
	isoal_status_t err = ISOAL_STATUS_ERR_SDU_ALLOC;

	if (sink->sdu_production.mode != ISOAL_PRODUCTION_MODE_DISABLED) {
		bool pdu_framed = (pdu_meta->pdu->cis.ll_id == PDU_BIS_LLID_FRAMED);

		if (pdu_framed) {
			err = isoal_rx_framed_consume(sink, pdu_meta);
		} else {
			err = isoal_rx_unframed_consume(sink, pdu_meta);
		}
	}

	return err;
}
