/*
 * IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING. By
 * downloading, copying, installing or using the software you agree to
 * this license. If you do not agree to this license, do not download,
 * install, copy or use the software.
 *
 * Intel License Agreement
 *
 * Copyright (c) 2000, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * -Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 * -Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the
 *  following disclaimer in the documentation and/or other materials
 *  provided with the distribution.
 *
 * -The name of Intel Corporation may not be used to endorse or
 *  promote products derived from this software
 *  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL INTEL
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */
#include "itd-config.h"

#include <sys/types.h>
#include <sys/param.h>

#include <stdlib.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include "iscsi.h"
#include "target.h"
#include "parameters.h"
#include "scsi_cmd_codes.h"

enum {
	TARGET_SHUT_DOWN = 0,
	TARGET_INITIALIZING = 1,
	TARGET_INITIALIZED = 2,
	TARGET_SHUTTING_DOWN = 3
};

/***********
 * Private *
 ***********/

static struct target_session *g_session;
static GList   *session_list;

/*********************
 * Private Functions *
 *********************/

static char *get_iqn(const struct target_session *sess, int t, char *buf,
		     size_t size)
{
	if (sess->globals->tv->v[t].iqn != NULL) {
		strlcpy(buf, sess->globals->tv->v[t].iqn, size);
		return buf;
	}
	snprintf(buf, size, "%s:%s", sess->globals->targetname,
		 sess->globals->tv->v[t].target);
	return buf;
}

static int reject_t(struct target_session *sess, const uint8_t * header,
		    uint8_t reason)
{
	struct iscsi_reject reject;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];

	iscsi_trace_error(__FILE__, __LINE__, "reject %x\n", reason);
	reject.reason = reason;
	reject.length = ISCSI_HEADER_LEN;
	reject.StatSN = ++(sess->StatSN);
	reject.ExpCmdSN = sess->ExpCmdSN;
	reject.MaxCmdSN = sess->MaxCmdSN;
	reject.DataSN = 0;	/* SNACK not yet implemented */

	if (iscsi_reject_encap(rsp_header, &reject) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_reject_encap() failed\n");
		return -1;
	}
	if (iscsi_sock_send_header_and_data(sess->conn, rsp_header,
					    ISCSI_HEADER_LEN, header,
					    ISCSI_HEADER_LEN, 0) !=
	    2 * ISCSI_HEADER_LEN) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_sock_send_header_and_data() failed\n");
		return -1;
	}
	return 0;
}

static int scsi_command_t(struct target_session *sess, const uint8_t * header)
{
	struct target_cmd cmd;
	struct iscsi_scsi_cmd_args scsi_cmd;
	struct iscsi_scsi_rsp scsi_rsp;
	struct iscsi_read_data data;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];
	uint32_t        DataSN = 0;

	memset(&scsi_cmd, 0x0, sizeof(scsi_cmd));
	if (iscsi_scsi_cmd_decap(header, &scsi_cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_scsi_cmd_decap() failed\n");
		return -1;
	}
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "session %d: SCSI Command (CmdSN %u, op %#x %s)\n", sess->id,
		    scsi_cmd.CmdSN,
		    scsi_cmd.cdb[0], sopstr(scsi_cmd.cdb[0]));

	/* For Non-immediate commands, the CmdSN should be between ExpCmdSN  */
	/* and MaxCmdSN, inclusive of both.  Otherwise, ignore the command */
	if ((!scsi_cmd.immediate) && ((scsi_cmd.CmdSN < sess->ExpCmdSN)
				      || (scsi_cmd.CmdSN > sess->MaxCmdSN))) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "CmdSN(%d) of SCSI Command not valid, ExpCmdSN(%d) MaxCmdSN(%d). Ignoring the command\n",
				  scsi_cmd.CmdSN, sess->ExpCmdSN,
				  sess->MaxCmdSN);
		return 0;
	}
	/* Arg check.   */
	scsi_cmd.attr = 0;	/* Temp fix FIXME */
	/*
	 * RETURN_NOT_EQUAL("ATTR (FIX ME)", scsi_cmd.attr, 0, NO_CLEANUP,
	 * -1);
	 */

	/* Check Numbering */

	if (scsi_cmd.CmdSN != sess->ExpCmdSN) {
		iscsi_trace_warning(__FILE__, __LINE__,
				    "Expected CmdSN %d, got %d. (ignoring and resetting expectations)\n",
				    sess->ExpCmdSN, scsi_cmd.CmdSN);
		sess->ExpCmdSN = scsi_cmd.CmdSN;
	}
	/* Check Transfer Lengths */
	if (sess->sess_params.first_burst
	    && (scsi_cmd.length > sess->sess_params.first_burst)) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "scsi_cmd.length (%u) > FirstBurstLength (%u)\n",
				  scsi_cmd.length,
				  sess->sess_params.first_burst);
		scsi_cmd.status = SCSI_CHECK_CONDITION;
		scsi_cmd.length = 0;
		goto response;
	}
	if (sess->sess_params.max_data_seg
	    && (scsi_cmd.length > sess->sess_params.max_data_seg)) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "scsi_cmd.length (%u) > MaxRecvDataSegmentLength (%u)\n",
				  scsi_cmd.length,
				  sess->sess_params.max_data_seg);
		return -1;
	}
#if 0
	/* commented out in original Intel reference code */
	if (scsi_cmd.final && scsi_cmd.output) {
		RETURN_NOT_EQUAL("Length", scsi_cmd.length, scsi_cmd.trans_len,
				 NO_CLEANUP, -1);
	}
#endif

	/* Read AHS.  Need to optimize/clean this.   */
	/* We should not be calling malloc(). */
	/* We need to check for properly formated AHS segments. */

	if (scsi_cmd.ahs_len) {
		uint32_t        ahs_len;
		uint8_t        *ahs_ptr;
		uint8_t         ahs_type;

		scsi_cmd.ahs = NULL;
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "reading %d bytes AHS\n", scsi_cmd.ahs_len);
		if ((scsi_cmd.ahs = malloc((unsigned)scsi_cmd.ahs_len)) == NULL) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "malloc() failed\n");
			return -1;
		}
#define AHS_CLEANUP do {						\
	free(scsi_cmd.ahs);			\
} while (/* CONSTCOND */ 0)

		memcpy(scsi_cmd.ahs, sess->ahs, scsi_cmd.ahs_len);

		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "read %d bytes AHS\n", scsi_cmd.ahs_len);
		for (ahs_ptr = scsi_cmd.ahs;
		     ahs_ptr < (scsi_cmd.ahs + scsi_cmd.ahs_len - 1);
		     ahs_ptr += ahs_len) {
			ahs_len = ntohs(*((uint16_t *) (void *)ahs_ptr));
			RETURN_EQUAL("AHS Length", ahs_len, 0, AHS_CLEANUP, -1);
			switch (ahs_type = *(ahs_ptr + 2)) {
			case ISCSI_AHS_EXTENDED_CDB:
				iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
					    __LINE__,
					    "Got ExtendedCDB AHS (%u bytes extra CDB)\n",
					    ahs_len - 1);
				scsi_cmd.ext_cdb = ahs_ptr + 4;
				break;
			case ISCSI_AHS_BIDI_READ:
				scsi_cmd.bidi_trans_len =
				    ntohl(*
					  ((uint32_t *) (void *)(ahs_ptr + 4)));
				*((uint32_t *) (void *)(ahs_ptr + 4)) =
				    scsi_cmd.bidi_trans_len;
				iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
					    __LINE__,
					    "Got Bidirectional Read AHS (expected read length %u)\n",
					    scsi_cmd.bidi_trans_len);
				break;
			default:
				iscsi_trace_error(__FILE__, __LINE__,
						  "unknown AHS type %x\n",
						  ahs_type);
				AHS_CLEANUP;
				return -1;
			}
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "done parsing %d bytes AHS\n", scsi_cmd.ahs_len);
	} else {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "no AHS to read\n");
		scsi_cmd.ahs = NULL;
	}

	sess->ExpCmdSN++;
	sess->MaxCmdSN++;

	/* Execute cdb.  device_command() will set scsi_cmd.input if */
	/* there is input data and set the length of the input */
	/* to either scsi_cmd.trans_len or scsi_cmd.bidi_trans_len, depending  */
	/* on whether scsi_cmd.output was set. */

	if (scsi_cmd.input) {
		scsi_cmd.send_data = sess->buff;
	}
	scsi_cmd.input = 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.scsi_cmd = &scsi_cmd;

	if (device_command(sess, &cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "device_command() failed\n");
		AHS_CLEANUP;
		return -1;
	}
	/* Send any input data */

	scsi_cmd.bytes_sent = 0;
	if (!scsi_cmd.status && scsi_cmd.input) {
		struct iovec    sg_singleton;
		struct iovec   *sg, *sg_orig, *sg_new = NULL;
		int             sg_len_orig, sg_len;
		uint32_t        offset, trans_len;
		int             fragment_flag = 0;
		int             offset_inc;
#define SG_CLEANUP do {							\
	if (fragment_flag) {						\
		free(sg_new);				\
	}								\
} while (/* CONSTCOND */ 0)
		if (scsi_cmd.output) {
			iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
				    "sending %u bytes bi-directional input data\n",
				    scsi_cmd.bidi_trans_len);
			trans_len = scsi_cmd.bidi_trans_len;
		} else {
			trans_len = scsi_cmd.trans_len;
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "sending %d bytes input data as separate PDUs\n",
			    trans_len);

		if (scsi_cmd.send_sg_len) {
			sg_orig = (struct iovec *)(void *)scsi_cmd.send_data;
			sg_len_orig = scsi_cmd.send_sg_len;
		} else {
			sg_len_orig = 1;
			sg_singleton.iov_base = scsi_cmd.send_data;
			sg_singleton.iov_len = trans_len;
			sg_orig = &sg_singleton;
		}
		sg = sg_orig;
		sg_len = sg_len_orig;

		offset_inc =
		    (sess->sess_params.max_data_seg) ? sess->sess_params.
		    max_data_seg : trans_len;

		for (offset = 0; offset < trans_len; offset += offset_inc) {
			memset(&data, 0x0, sizeof(data));

			data.length =
			    (sess->sess_params.max_data_seg) ? MIN(trans_len -
								   offset,
								   sess->sess_params.
								   max_data_seg)
			    : trans_len - offset;
			if (data.length != trans_len) {
				if (!fragment_flag) {
					if ((sg_new =
					     malloc(sizeof(struct iovec)
						    * sg_len_orig))
					    == NULL) {
						iscsi_trace_error(__FILE__,
								  __LINE__,
								  "malloc() failed\n");
						AHS_CLEANUP;
						return -1;
					}
					fragment_flag++;
				}
				sg = sg_new;
				sg_len = sg_len_orig;
				memcpy(sg, sg_orig,
				       sizeof(struct iovec) * sg_len_orig);
				if (modify_iov
				    (&sg, &sg_len, offset, data.length) != 0) {
					iscsi_trace_error(__FILE__, __LINE__,
							  "modify_iov() failed\n");
					SG_CLEANUP;
					AHS_CLEANUP;
					return -1;
				}
			}
			iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
				    "sending read data PDU (offset %u, len %u)\n",
				    offset, data.length);
			if (offset + data.length == trans_len) {
				data.final = 1;

				if (sess->UsePhaseCollapsedRead) {
					data.status = 1;
					data.status = scsi_cmd.status;
					data.StatSN = ++(sess->StatSN);
					iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
						    __LINE__,
						    "status %#x collapsed into last data PDU\n",
						    data.status);
				} else {
					iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
						    __LINE__,
						    "NOT collapsing status with last data PDU\n");
				}
			} else if (offset + data.length > trans_len) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "offset+data.length > trans_len??\n");
				SG_CLEANUP;
				AHS_CLEANUP;
				return -1;
			}
			data.task_tag = scsi_cmd.tag;
			data.ExpCmdSN = sess->ExpCmdSN;
			data.MaxCmdSN = sess->MaxCmdSN;
			data.DataSN = DataSN++;
			data.offset = offset;
			if (iscsi_read_data_encap(rsp_header, &data) != 0) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "iscsi_read_data_encap() failed\n");
				SG_CLEANUP;
				AHS_CLEANUP;
				return -1;
			}
			if (iscsi_sock_send_header_and_data
			    (sess->conn, rsp_header, ISCSI_HEADER_LEN, sg,
			     data.length, sg_len)
			    != ISCSI_HEADER_LEN + data.length) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "iscsi_sock_send_header_and_data() failed\n");
				SG_CLEANUP;
				AHS_CLEANUP;
				return -1;
			}
			scsi_cmd.bytes_sent += data.length;
			iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
				    "sent read data PDU ok (offset %u, len %u)\n",
				    data.offset, data.length);
		}
		SG_CLEANUP;
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "successfully sent %d bytes read data\n",
			    trans_len);
	}
	/*
	 * Send a response PDU if
	 *
	 * 1) we're not using phase collapsed input (and status was good)
	 * 2) we are using phase collapsed input, but there was no input data (e.g., TEST UNIT READY)
	 * 3) command had non-zero status and possible sense data
	 */
response:
	if (!sess->UsePhaseCollapsedRead || !scsi_cmd.length || scsi_cmd.status) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "sending SCSI response PDU\n");
		memset(&scsi_rsp, 0x0, sizeof(scsi_rsp));
		scsi_rsp.length = scsi_cmd.status ? scsi_cmd.length : 0;
		scsi_rsp.tag = scsi_cmd.tag;
		/* If r2t send, then the StatSN is already incremented */
		if (sess->StatSN < scsi_cmd.ExpStatSN) {
			++sess->StatSN;
		}
		scsi_rsp.StatSN = sess->StatSN;
		scsi_rsp.ExpCmdSN = sess->ExpCmdSN;
		scsi_rsp.MaxCmdSN = sess->MaxCmdSN;
		scsi_rsp.ExpDataSN = (!scsi_cmd.status
				      && scsi_cmd.input) ? DataSN : 0;
		scsi_rsp.response = 0x00;	/* iSCSI response */
		scsi_rsp.status = scsi_cmd.status;	/* SCSI status */
		if (iscsi_scsi_rsp_encap(rsp_header, &scsi_rsp) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_scsi_rsp_encap() failed\n");
			AHS_CLEANUP;
			return -1;
		}
		if (iscsi_sock_send_header_and_data(sess->conn, rsp_header,
						    ISCSI_HEADER_LEN,
						    scsi_cmd.send_data,
						    scsi_rsp.length,
						    scsi_cmd.send_sg_len)
		    != ISCSI_HEADER_LEN + scsi_rsp.length) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_sock_send_header_and_data() failed\n");
			AHS_CLEANUP;
			return -1;
		}
		/* Make sure all data was transferred */

		if (scsi_cmd.output) {
			scsi_cmd.bytes_recv = sess->xfer.bytes_recv;
			RETURN_NOT_EQUAL("scsi_cmd.bytes_recv",
					 scsi_cmd.bytes_recv,
					 scsi_cmd.trans_len, AHS_CLEANUP, -1);

			if (scsi_cmd.input) {
				RETURN_NOT_EQUAL("scsi_cmd.bytes_sent",
						 scsi_cmd.bytes_sent,
						 scsi_cmd.bidi_trans_len,
						 AHS_CLEANUP, -1);
			}
		} else {
			if (scsi_cmd.input) {
				RETURN_NOT_EQUAL("scsi_cmd.bytes_sent",
						 scsi_cmd.bytes_sent,
						 scsi_cmd.trans_len,
						 AHS_CLEANUP, -1);
			}
		}
	}
	/* Device callback after command has completed */

	if (cmd.callback) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "issuing device callback\n");
		if ((*cmd.callback) (cmd.callback_arg) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "device callback failed\n");
			AHS_CLEANUP;
			return -1;
		}
	}
	AHS_CLEANUP;
	return 0;
}

static int task_command_t(struct target_session *sess, const uint8_t *header)
{
	struct iscsi_task_cmd cmd;
	struct iscsi_task_rsp rsp;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];

	/* Get & check args */

	if (iscsi_task_cmd_decap(header, &cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_task_cmd_decap() failed\n");
		return -1;
	}
	if (cmd.CmdSN != sess->ExpCmdSN) {
		iscsi_trace_warning(__FILE__, __LINE__,
				    "Expected CmdSN %d, got %d. (ignoring and resetting expectations)\n",
				    cmd.CmdSN, sess->ExpCmdSN);
		sess->ExpCmdSN = cmd.CmdSN;
	}
	sess->MaxCmdSN++;

	memset(&rsp, 0x0, sizeof(rsp));
	rsp.response = ISCSI_TASK_RSP_FUNCTION_COMPLETE;

	switch (cmd.function) {
	case ISCSI_TASK_CMD_ABORT_TASK:
		printf("ISCSI_TASK_CMD_ABORT_TASK\n");
		break;
	case ISCSI_TASK_CMD_ABORT_TASK_SET:
		printf("ISCSI_TASK_CMD_ABORT_TASK_SET\n");
		break;
	case ISCSI_TASK_CMD_CLEAR_ACA:
		printf("ISCSI_TASK_CMD_CLEAR_ACA\n");
		break;
	case ISCSI_TASK_CMD_CLEAR_TASK_SET:
		printf("ISCSI_TASK_CMD_CLEAR_TASK_SET\n");
		break;
	case ISCSI_TASK_CMD_LOGICAL_UNIT_RESET:
		printf("ISCSI_TASK_CMD_LOGICAL_UNIT_RESET\n");
		break;
	case ISCSI_TASK_CMD_TARGET_WARM_RESET:
		printf("ISCSI_TASK_CMD_TARGET_WARM_RESET\n");
		break;
	case ISCSI_TASK_CMD_TARGET_COLD_RESET:
		printf("ISCSI_TASK_CMD_TARGET_COLD_RESET\n");
		break;
	case ISCSI_TASK_CMD_TARGET_REASSIGN:
		printf("ISCSI_TASK_CMD_TARGET_REASSIGN\n");
		break;
	default:
		iscsi_trace_error(__FILE__, __LINE__,
				  "Unknown task function %d\n", cmd.function);
		rsp.response = ISCSI_TASK_RSP_REJECTED;
	}

	rsp.tag = cmd.tag;
	rsp.StatSN = ++(sess->StatSN);
	rsp.ExpCmdSN = sess->ExpCmdSN;
	rsp.MaxCmdSN = sess->MaxCmdSN;

	if (iscsi_task_rsp_encap(rsp_header, &rsp) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_task_cmd_decap() failed\n");
		return -1;
	}

	gnet_conn_write(sess->conn, (gchar *) rsp_header, ISCSI_HEADER_LEN);

	return 0;
}

static int nop_out_t(struct target_session *sess, const uint8_t *header)
{
	struct iscsi_nop_out_args nop_out;

	if (iscsi_nop_out_decap(header, &nop_out) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_nop_out_decap() failed\n");
		return -1;
	}
	if (nop_out.CmdSN != sess->ExpCmdSN) {
		iscsi_trace_warning(__FILE__, __LINE__,
				    "Expected CmdSN %d, got %d. (ignoring and resetting expectations)\n",
				    nop_out.CmdSN, sess->ExpCmdSN);
		sess->ExpCmdSN = nop_out.CmdSN;
	}
	/* TODO Clarify whether we need to update the CmdSN */
	/* sess->ExpCmdSN++;  */
	/* sess->MaxCmdSN++;  */

	if (nop_out.length) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "successfully read %d bytes ping data:\n",
			    nop_out.length);
		iscsi_print_buffer(sess->buff, nop_out.length);
	}
	if (nop_out.tag != 0xffffffff) {
		struct iscsi_nop_in_args nop_in;
		uint8_t         rsp_header[ISCSI_HEADER_LEN];

		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "sending %d bytes ping response\n", nop_out.length);
		memset(&nop_in, 0x0, sizeof(&nop_in));
		nop_in.length = nop_out.length;
		nop_in.lun = nop_out.lun;
		nop_in.tag = nop_out.tag;
		nop_in.transfer_tag = 0xffffffff;
		nop_in.StatSN = ++(sess->StatSN);
		nop_in.ExpCmdSN = sess->ExpCmdSN;
		nop_in.MaxCmdSN = sess->MaxCmdSN;

		if (iscsi_nop_in_encap(rsp_header, &nop_in) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_nop_in_encap() failed\n");
			return -1;
		}
		if (iscsi_sock_send_header_and_data(sess->conn, rsp_header,
						    ISCSI_HEADER_LEN,
						    sess->buff, nop_in.length,
						    0) !=
		    ISCSI_HEADER_LEN + nop_in.length) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_sock_send_header_and_data() failed\n");
			return -1;
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "successfully sent %d bytes ping response\n",
			    nop_out.length);
	}
	return 0;
}

/*
 * text_command_t
 */

static int text_command_t(struct target_session *sess, const uint8_t *header)
{
	struct iscsi_text_cmd_args text_cmd;
	struct iscsi_text_rsp_args text_rsp;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];
	char           *text_in = NULL;
	char           *text_out = NULL;
	unsigned        len_in;
	char            buf[BUFSIZ];
	int             len_out = 0;
	int             i;

#define TC_CLEANUP do {							\
	free(text_in);				\
	free(text_out);				\
} while (/* CONSTCOND */ 0)
#define TC_ERROR {							\
	TC_CLEANUP;							\
	return -1;							\
}
	/* Get text args */

	if (iscsi_text_cmd_decap(header, &text_cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_text_cmd_decap() failed\n");
		return -1;
	}
	/* Check args & update numbering */
	RETURN_NOT_EQUAL("Continue", text_cmd.cont, 0, NO_CLEANUP, -1);
	RETURN_NOT_EQUAL("CmdSN", text_cmd.CmdSN, sess->ExpCmdSN, NO_CLEANUP,
			 -1);

	sess->ExpCmdSN++;
	sess->MaxCmdSN++;

	if ((text_out = malloc(2048)) == NULL) {
		iscsi_trace_error(__FILE__, __LINE__, "malloc() failed\n");
		return -1;
	}
	/* Read text parameters */

	if ((len_in = text_cmd.length) != 0) {
		struct iscsi_parameter *ptr;

		if ((text_in = malloc(len_in + 1)) == NULL) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "malloc() failed\n");
			TC_CLEANUP;
			return -1;
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "reading %d bytes text parameters\n", len_in);

		memcpy(text_in, sess->buff, len_in);

		text_in[len_in] = 0x0;
		PARAM_TEXT_PARSE(sess->params, &sess->sess_params.cred, text_in,
				 (int)len_in, text_out, (int *)(void *)&len_out,
				 2048, 0, TC_ERROR);

		/*
		 * Handle exceptional cases not covered by parameters.c
		 * (e.g., SendTargets)
		 */

		if ((ptr = param_get(sess->params, "SendTargets")) == NULL) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "param_get() failed\n");
			TC_CLEANUP;
			return -1;
		}
		if (ptr->rx_offer) {
			if (ptr->offer_rx && strcmp(ptr->offer_rx, "All") == 0
			    && !param_equiv(sess->params, "SessionType",
					    "Discovery")) {
				iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
					    __LINE__,
					    "Rejecting SendTargets=All in a non Discovery session\n");
				PARAM_TEXT_ADD(sess->params, "SendTargets",
					       "Reject", text_out, &len_out,
					       2048, 0, TC_ERROR);
			} else {
				for (i = 0; i < sess->globals->tv->c; i++) {
					if (sess->address_family == ISCSI_IPv6
					    || (sess->address_family ==
						ISCSI_IPv4
						&& allow_netmask(sess->globals->
								 tv->v[i].mask,
								 sess->
								 initiator))) {
						get_iqn(sess, i, buf,
							sizeof(buf));
						PARAM_TEXT_ADD(sess->params,
							       "TargetName",
							       buf, text_out,
							       &len_out, 2048,
							       0, TC_ERROR);
						PARAM_TEXT_ADD(sess->params,
							       "TargetAddress",
							       sess->globals->
							       targetaddress,
							       text_out,
							       &len_out, 2048,
							       0, TC_ERROR);
					} else {
#ifdef HAVE_SYSLOG_H
						syslog(LOG_INFO,
						       "WARNING: attempt to discover targets from %s (not allowed by %s) has been rejected",
						       sess->initiator,
						       sess->globals->tv->v[0].
						       mask);
#endif
					}
				}
			}
			ptr->rx_offer = 0;
		}
		/* Parse outgoing offer */

		if (len_out) {
			PARAM_TEXT_PARSE(sess->params, &sess->sess_params.cred,
					 text_out, len_out, NULL, NULL, 2048, 1,
					 TC_ERROR);
		}
	}
	if (sess->IsFullFeature) {
		set_session_parameters(sess->params, &sess->sess_params);
	}
	/* Send response */

	text_rsp.final = text_cmd.final;
	text_rsp.cont = 0;
	text_rsp.length = len_out;
	text_rsp.lun = text_cmd.lun;
	text_rsp.tag = text_cmd.tag;
	text_rsp.transfer_tag = (text_rsp.final) ? 0xffffffff : 0x1234;
	text_rsp.StatSN = ++(sess->StatSN);
	text_rsp.ExpCmdSN = sess->ExpCmdSN;
	text_rsp.MaxCmdSN = sess->MaxCmdSN;
	if (iscsi_text_rsp_encap(rsp_header, &text_rsp) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_text_rsp_encap() failed\n");
		TC_CLEANUP;
		return -1;
	}

	gnet_conn_write(sess->conn, (gchar *) rsp_header, ISCSI_HEADER_LEN);

	if (len_out) {
		gnet_conn_write(sess->conn, text_out, len_out);
		send_padding(sess->conn, len_out);
	}

	TC_CLEANUP;
	return 0;
}

/* given a target's iqn, find the relevant target that we're exporting */
static int find_target_iqn(struct target_session *sess)
{
	char            buf[BUFSIZ];
	int             i;

	for (i = 0; i < sess->globals->tv->c; i++)
		if (param_equiv(sess->params, "TargetName",
				get_iqn(sess, i, buf, sizeof(buf)))) {
			sess->d = i;
			return i;
		}

	return -1;
}

/* given a tsih, find the relevant target that we're exporting */
static int find_target_tsih(struct globals *globals, int tsih)
{
	int             i;

	for (i = 0; i < globals->tv->c; i++)
		if (globals->tv->v[i].tsih == tsih)
			return i;

	return -1;
}

/*
 * login_command_t() handles login requests and replies.
 */

static int login_command_t(struct target_session *sess, const uint8_t *header)
{
	struct iscsi_login_cmd_args cmd;
	struct iscsi_login_rsp_args rsp;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];
	char           *text_in = NULL;
	char           *text_out = NULL;
	char            logbuf[BUFSIZ];
	int             len_in = 0;
	int             len_out = 0;
	int             status = 0;
	int             i;

	/* Initialize response */

#define LC_CLEANUP do {							\
	free(text_in);				\
	free(text_out);				\
} while (/* CONSTCOND */ 0)
#define LC_ERROR {							\
	TC_CLEANUP;							\
	return -1;							\
}

	memset(&rsp, 0x0, sizeof(rsp));
	rsp.status_class = ISCSI_LOGIN_STATUS_INITIATOR_ERROR;

	/* Get login args & check preconditions */

	if (iscsi_login_cmd_decap(header, &cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_login_cmd_decap() failed\n");
		goto response;
	}
	if (sess->IsLoggedIn) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "duplicate login attempt on sess %d\n",
				  sess->id);
		goto response;
	}
	if ((cmd.cont != 0) && (cmd.transit != 0)) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "Bad cmd.continue.  Expected 0.\n");
		goto response;
	} else if ((cmd.version_max < ISCSI_VERSION)
		   || (cmd.version_min > ISCSI_VERSION)) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "Target iscsi version (%u) not supported by initiator [Max Ver (%u) and Min Ver (%u)]\n",
				  ISCSI_VERSION, cmd.version_max,
				  cmd.version_min);
		rsp.status_class = ISCSI_LOGIN_STATUS_INITIATOR_ERROR;
		rsp.status_detail = ISCSI_LOGIN_DETAIL_VERSION_NOT_SUPPORTED;
		rsp.version_max = ISCSI_VERSION;
		rsp.version_active = ISCSI_VERSION;
		goto response;
	} else if (cmd.tsih != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "Bad cmd.tsih (%u). Expected 0.\n", cmd.tsih);
		goto response;
	}
	/* Parse text parameters and build response */

	if ((text_out = malloc(2048)) == NULL) {
		iscsi_trace_error(__FILE__, __LINE__, "malloc() failed\n");
		return -1;
	}
	if ((len_in = cmd.length) != 0) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "reading %d bytes text data\n", len_in);
		if ((text_in = malloc((unsigned)(len_in + 1))) == NULL) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "malloc() failed\n");
			LC_CLEANUP;
			return -1;
		}

		memcpy(text_in, sess->buff, len_in);

		text_in[len_in] = 0x0;
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "successfully read %d bytes text data\n", len_in);

		/*
		 * Parse incoming parameters (text_out will contain the
		 * response we need
		 */

		/* to send back to the initiator */

		if ((status =
		     param_text_parse(sess->params, &sess->sess_params.cred,
				      text_in, len_in, text_out, &len_out, 2048,
				      0)) != 0) {
			switch (status) {
			case ISCSI_PARAM_STATUS_FAILED:
				rsp.status_detail = ISCSI_LOGIN_DETAIL_SUCCESS;
				break;
			case ISCSI_PARAM_STATUS_AUTH_FAILED:
				rsp.status_detail =
				    ISCSI_LOGIN_DETAIL_INIT_AUTH_FAILURE;
				break;
			default:
				/*
				 * We will need to set the detail field based on more detailed error
				 * cases. Will need to fix this if compliciance test break
				 * (status_detail field).
				 */
				break;
			}
			goto response;
		}
		/* Parse the outgoing offer */
		if (!sess->LoginStarted) {
			PARAM_TEXT_ADD(sess->params, "TargetPortalGroupTag",
				       "1", text_out, &len_out, 2048, 0,
				       LC_ERROR);
		}
		if (len_out) {
			PARAM_TEXT_PARSE(sess->params, &sess->sess_params.cred,
					 text_out, len_out, NULL, NULL, 2048, 1,
					 LC_ERROR;);
		}
	}
	if (!sess->LoginStarted) {
		sess->LoginStarted = 1;
	}
	/*
	 * For now, we accept what ever the initiators' current and next
	 * states are. And le are always
	 */
	/* ready to transitition to that state. */

	rsp.csg = cmd.csg;
	rsp.nsg = cmd.nsg;
	rsp.transit = cmd.transit;

	if (cmd.csg == ISCSI_LOGIN_STAGE_SECURITY) {
		if (param_equiv(sess->params, "AuthResult", "No")) {
			rsp.transit = 0;
		} else if (param_equiv(sess->params, "AuthResult", "Fail")) {
			rsp.status_class = rsp.status_detail =
			    ISCSI_LOGIN_DETAIL_INIT_AUTH_FAILURE;
			goto response;
		}
	}
	if (cmd.transit && cmd.nsg == ISCSI_LOGIN_STAGE_FULL_FEATURE) {

		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "transitioning to ISCSI_LOGIN_STAGE_FULL_FEATURE\n");

		/* Check post conditions */

		if (param_equiv(sess->params, "InitiatorName", "")) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "InitiatorName not specified\n");
			goto response;
		}
		if (param_equiv(sess->params, "SessionType", "Normal")) {
			if (param_equiv(sess->params, "TargetName", "")) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "TargetName not specified\n");
				goto response;
			}
			if ((i = find_target_iqn(sess)) < 0) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "Bad TargetName \"%s\"\n",
						  param_val(sess->params,
							    "TargetName"));
				goto response;
			}
			if (cmd.tsih != 0
			    && find_target_tsih(sess->globals, cmd.tsih) != i) {
				iscsi_trace_error(__FILE__, __LINE__,
						  "target tsih expected %d, cmd.tsih %d, i %d\n",
						  sess->globals->tv->v[i].tsih,
						  cmd.tsih, i);
			}
			sess->d = i;
		} else if ((i = find_target_tsih(sess->globals, cmd.tsih)) < 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "Abnormal SessionType cmd.tsih %d not found\n",
					  cmd.tsih);
		}
		if (param_equiv(sess->params, "SessionType", "")) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "SessionType not specified\n");
			goto response;
		}
		sess->ExpCmdSN = sess->MaxCmdSN = cmd.CmdSN;
		sess->cid = cmd.cid;
		sess->isid = cmd.isid;

		sess->globals->tv->v[i].tsih = sess->tsih =
		    ++sess->globals->last_tsih;
		sess->IsFullFeature = 1;

		sess->IsLoggedIn = 1;
		if (!param_equiv(sess->params, "SessionType", "Discovery")) {
			strlcpy(param_val(sess->params, "MaxConnections"), "1",
				2);
		}
		set_session_parameters(sess->params, &sess->sess_params);
	} else {
		if ((i = find_target_tsih(sess->globals, cmd.tsih)) < 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "cmd.tsih %d not found\n", cmd.tsih);
		}
	}

	/* No errors */

	rsp.status_class = rsp.status_detail = ISCSI_LOGIN_DETAIL_SUCCESS;
	rsp.length = len_out;

	/* Send login response */

response:
	sess->ExpCmdSN = sess->MaxCmdSN = cmd.CmdSN;
	rsp.isid = cmd.isid;
	rsp.StatSN = cmd.ExpStatSN;	/* debug  */
	rsp.tag = cmd.tag;
	rsp.cont = cmd.cont;
	rsp.ExpCmdSN = sess->ExpCmdSN;
	rsp.MaxCmdSN = sess->MaxCmdSN;
	if (!rsp.status_class) {
		if (rsp.transit && (rsp.nsg == ISCSI_LOGIN_STAGE_FULL_FEATURE)) {
			rsp.version_max = ISCSI_VERSION;
			rsp.version_active = ISCSI_VERSION;
			rsp.StatSN = ++(sess->StatSN);
			rsp.tsih = sess->tsih;
		}
	}
	if (iscsi_login_rsp_encap(rsp_header, &rsp) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_login_rsp_encap() failed\n");
		LC_CLEANUP;
		return -1;
	}
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "sending login response\n");
	if (iscsi_sock_send_header_and_data(sess->conn, rsp_header,
					    ISCSI_HEADER_LEN, text_out,
					    rsp.length,
					    0) !=
	    ISCSI_HEADER_LEN + rsp.length) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_sock_send_header_and_data() failed\n");
		LC_CLEANUP;
		return -1;
	}
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "sent login response ok\n");
	if (rsp.status_class != 0) {
		LC_CLEANUP;
		return -1;
	}
	if (cmd.transit && cmd.nsg == ISCSI_LOGIN_STAGE_FULL_FEATURE) {

		/* log information to stdout */
		snprintf(logbuf, sizeof(logbuf),
			 "> iSCSI %s login  successful from %s on %s disk %d, ISID %"
			 PRIu64 ", TSIH %u", param_val(sess->params,
						       "SessionType"),
			 param_val(sess->params, "InitiatorName"),
			 sess->initiator, sess->d, sess->isid, sess->tsih);
		printf("%s\n", logbuf);
#ifdef HAVE_SYSLOG_H
		/* log information to syslog */
		syslog(LOG_INFO, "%s", logbuf);
#endif

		/* Buffer for data xfers to/from the scsi device */
		if (!param_equiv(sess->params, "MaxRecvDataSegmentLength", "0")) {
			/* do nothing */
		} else {
			iscsi_trace_error(__FILE__, __LINE__,
					  "MaxRecvDataSegmentLength of 0 not supported\n");
			LC_CLEANUP;
			return -1;
		}
	}
	LC_CLEANUP;
	return 0;
}

static int logout_command_t(struct target_session *sess, const uint8_t *header)
{
	struct iscsi_logout_cmd_args cmd;
	struct iscsi_logout_rsp_args rsp;
	uint8_t         rsp_header[ISCSI_HEADER_LEN];
	char            logbuf[BUFSIZ];
	int             i;

	memset(&rsp, 0x0, sizeof(rsp));
	if (iscsi_logout_cmd_decap(header, &cmd) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_logout_cmd_decap() failed\n");
		return -1;
	}
	sess->StatSN = cmd.ExpStatSN;
	if ((cmd.reason == ISCSI_LOGOUT_CLOSE_RECOVERY)
	    && (param_equiv(sess->params, "ErrorRecoveryLevel", "0"))) {
		rsp.response = ISCSI_LOGOUT_STATUS_NO_RECOVERY;
	}
	RETURN_NOT_EQUAL("CmdSN", cmd.CmdSN, sess->ExpCmdSN, NO_CLEANUP, -1);
	RETURN_NOT_EQUAL("ExpStatSN", cmd.ExpStatSN, sess->StatSN, NO_CLEANUP,
			 -1);

	rsp.tag = cmd.tag;
	rsp.StatSN = sess->StatSN;
	rsp.ExpCmdSN = ++sess->ExpCmdSN;
	rsp.MaxCmdSN = sess->MaxCmdSN;
	if (iscsi_logout_rsp_encap(rsp_header, &rsp) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_logout_rsp_encap() failed\n");
		return -1;
	}

	gnet_conn_write(sess->conn, (gchar *) rsp_header, ISCSI_HEADER_LEN);

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "sent logout response OK\n");

	/* log information to stdout */
	snprintf(logbuf, sizeof(logbuf),
		 "< iSCSI %s logout successful from %s on %s disk %d, ISID %"
		 PRIu64 ", TSIH %u", param_val(sess->params,
					       "SessionType"),
		 param_val(sess->params, "InitiatorName"),
		 sess->initiator, sess->d, sess->isid, sess->tsih);
	printf("%s\n", logbuf);
#ifdef HAVE_SYSLOG
	/* log information to syslog */
	syslog(LOG_INFO, "%s", logbuf);
#endif

	sess->IsLoggedIn = 0;

	if (sess->sess_params.cred.user) {
		free(sess->sess_params.cred.user);
		sess->sess_params.cred.user = NULL;
	}

	if ((i = find_target_tsih(sess->globals, sess->tsih)) < 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "logout sess->tsih %d not found\n",
				  sess->tsih);
	} else {
		sess->globals->tv->v[i].tsih = 0;
	}
	sess->tsih = 0;

	return 0;
}

static int verify_cmd_t(struct target_session *sess, const uint8_t *header)
{
	int             op = ISCSI_OPCODE(header);

	if ((!sess->LoginStarted) && (op != ISCSI_LOGIN_CMD)) {
		/* Terminate the connection */
		iscsi_trace_error(__FILE__, __LINE__,
				  "session %d: iSCSI op %#x attempted before LOGIN PHASE\n",
				  sess->id, op);
		return -1;
	}
	if (!sess->IsFullFeature
	    && ((op != ISCSI_LOGIN_CMD) && (op != ISCSI_LOGOUT_CMD))) {
		struct iscsi_login_rsp_args rsp;
		uint8_t         rsp_header[ISCSI_HEADER_LEN];
		iscsi_trace_error(__FILE__, __LINE__,
				  "session %d: iSCSI op %#x attempted before FULL FEATURE\n",
				  sess->id, op);
		/* Create Login Reject response */
		memset(&rsp, 0x0, sizeof(rsp));
		rsp.status_class = ISCSI_LOGIN_STATUS_INITIATOR_ERROR;
		rsp.status_detail = ISCSI_LOGIN_DETAIL_NOT_LOGGED_IN;
		rsp.version_max = ISCSI_VERSION;
		rsp.version_active = ISCSI_VERSION;

		if (iscsi_login_rsp_encap(rsp_header, &rsp) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_login_rsp_encap() failed\n");
			return -1;
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "sending login response\n");
		if (iscsi_sock_send_header_and_data(sess->conn, rsp_header,
						    ISCSI_HEADER_LEN, NULL, 0,
						    0) !=
		    ISCSI_HEADER_LEN + rsp.length) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "iscsi_sock_send_header_and_data() failed\n");
			return -1;
		}
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "sent login response ok\n");
		return -1;
	}
	return 0;
}

/*
 * this function looks at the opcode in the received header for the session,
 * and does a switch on the opcode to call the required function.
 */
static int execute_t(struct target_session *sess, const uint8_t *header)
{
	int             op = ISCSI_OPCODE(header);

	if (verify_cmd_t(sess, header) != 0) {
		return -1;
	}
	switch (op) {
	case ISCSI_TASK_CMD:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: Task Command\n", sess->id);
		if (task_command_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "task_command_t() failed\n");
			return -1;
		}
		break;

	case ISCSI_NOP_OUT:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: NOP-Out\n", sess->id);
		if (nop_out_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "nop_out_t() failed\n");
			return -1;
		}
		break;

	case ISCSI_LOGIN_CMD:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: Login Command\n", sess->id);
		if (login_command_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "login_command_t() failed\n");
			return -1;
		}
		break;

	case ISCSI_TEXT_CMD:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: Text Command\n", sess->id);
		if (text_command_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "text_command_t() failed\n");
			return -1;
		}
		break;

	case ISCSI_LOGOUT_CMD:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: Logout Command\n", sess->id);
		if (logout_command_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "logout_command_t() failed\n");
			return -1;
		}
		break;

	case ISCSI_SCSI_CMD:
		iscsi_trace(TRACE_ISCSI_CMD, __FILE__, __LINE__,
			    "session %d: SCSI Command\n", sess->id);
		if (scsi_command_t(sess, header) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "scsi_command_t() failed\n");
			return -1;
		}
		break;

	default:
		iscsi_trace_error(__FILE__, __LINE__, "Unknown Opcode %#x\n",
				  ISCSI_OPCODE(header));
		if (reject_t(sess, header, 0x04) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "reject_t() failed\n");
			return -1;
		}
		break;
	}
	return 0;
}

static int
read_data_pdu(struct target_session *sess,
	      struct iscsi_write_data *data)
{
	uint8_t         header[ISCSI_HEADER_LEN];
	int             ret_val = -1;

	memcpy(header, sess->header, sizeof(header));

	if ((ret_val = iscsi_write_data_decap(header, data)) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "iscsi_write_data_decap() failed\n");
		return ret_val;
	}

	/* Check args */
	if (sess->sess_params.max_data_seg) {
		if (data->length > sess->sess_params.max_data_seg) {
			sess->xfer.status = SCSI_CHECK_CONDITION;
			return -1;
		}
	}
	if ((sess->xfer.bytes_recv + data->length) > sess->xfer.trans_len) {
		sess->xfer.status = SCSI_CHECK_CONDITION;
		return -1;
	}
	if (data->tag != sess->xfer.tag) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "Data ITT (%d) does not match with command ITT (%d)\n",
			    data->tag, sess->xfer.tag);
		if (data->final) {
			sess->xfer.status = SCSI_CHECK_CONDITION;
			return -1;
		} else {
			/* Send a reject PDU */
			iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
				    "Sending Reject PDU\n");
			if (reject_t(sess, header, 0x09) != 0) {	/* Invalid PDU Field */
				iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
					    __LINE__,
					    "Sending Reject PDU failed\n");
				return 1;
			}
		}
	}
	return 0;
}

static int send_r2t(struct target_session *sess)
{
	int send_it = 0;
	uint8_t         header[ISCSI_HEADER_LEN];

	/*
	 * Send R2T if we're either operating in solicted
	 * mode or we're operating in unsolicted
	 */
	/* mode and have reached the first burst */
	if (!sess->xfer.r2t_flag && (sess->sess_params.initial_r2t ||
			  (sess->sess_params.first_burst
			   && (sess->xfer.bytes_recv >=
			       sess->sess_params.first_burst))))
		send_it = 1;

	if (!send_it)
		return 0;

	sess->xfer.desired_len =
	    MIN((sess->xfer.trans_len - sess->xfer.bytes_recv),
		sess->sess_params.max_burst);

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "sending R2T for %u bytes data\n",
		    sess->xfer.desired_len);

	sess->xfer.r2t.tag = sess->xfer.tag;

	sess->xfer.r2t.transfer_tag = 0x1234;

	sess->xfer.r2t.ExpCmdSN = sess->ExpCmdSN;
	sess->xfer.r2t.MaxCmdSN = sess->MaxCmdSN;
	sess->xfer.r2t.StatSN = ++(sess->StatSN);
	sess->xfer.r2t.length = sess->xfer.desired_len;
	sess->xfer.r2t.offset = sess->xfer.bytes_recv;

	if (iscsi_r2t_encap(header, &sess->xfer.r2t) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "r2t_encap() failed\n");
		return -1;
	}

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
		    __LINE__,
		    "sending R2T tag %u transfer tag %u len %u offset %u\n",
		    sess->xfer.r2t.tag, sess->xfer.r2t.transfer_tag,
		    sess->xfer.r2t.length, sess->xfer.r2t.offset);

	gnet_conn_write(sess->conn, (gchar *) header,
			ISCSI_HEADER_LEN);

	sess->xfer.r2t_flag = 1;
	sess->xfer.r2t.R2TSN += 1;

	return 0;
}

#define TTD_CLEANUP do {						\
	free(iov_ptr);				\
} while (/* CONSTCOND */ 0)

static int target_data_pdu(struct target_session *sess)
{
	struct iscsi_write_data data;
	int read_status;
	struct iovec   *iov = NULL, *iov_ptr = NULL;

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "reading data pdu\n");
	if ((read_status = read_data_pdu(sess, &data)) != 0) {
		if (read_status == 1) {
			iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__,
				    __LINE__,
		    "Unknown PDU received and ignored.  Expecting Data PDU\n");
			return 1;
		} else {
			iscsi_trace_error(__FILE__, __LINE__,
					  "read_data_pdu() failed\n");
			sess->xfer.status = SCSI_CHECK_CONDITION;
			return -1;
		}
	}
	WARN_NOT_EQUAL("ExpStatSN", data.ExpStatSN, sess->StatSN);
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "read data pdu OK (offset %u, length %u)\n",
		    data.offset, data.length);

	/* Scatter into destination buffers */

	/* FIXME - VERY wrong - just a placeholder */
	memcpy(iov[0].iov_base, sess->buff, data.length);
	iov[0].iov_len -= data.length;

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "successfully scattered %u bytes\n", data.length);

	sess->xfer.bytes_recv += data.length;
	sess->xfer.desired_len -= data.length;

	if ((!sess->xfer.r2t_flag)
	    && (sess->xfer.bytes_recv > sess->sess_params.first_burst)) {
		iscsi_trace_error(__FILE__, __LINE__,
		  "Received unsolicited data (%d) more than first_burst (%d)\n",
				  sess->xfer.bytes_recv,
				  sess->sess_params.first_burst);
		sess->xfer.status = SCSI_CHECK_CONDITION;
		return -1;
	}
	if ((sess->xfer.desired_len != 0) && data.final) {
		iscsi_trace_error(__FILE__, __LINE__,
		  "Expecting more data (%d) from initiator for this sequence\n",
				  sess->xfer.desired_len);
		sess->xfer.status = SCSI_CHECK_CONDITION;
		return -1;
	}
	if ((sess->xfer.desired_len == 0) && !data.final) {
		iscsi_trace_error(__FILE__, __LINE__,
		  "Final bit not set on the last data PDU of this sequence\n");
		sess->xfer.status = SCSI_CHECK_CONDITION;
		return -1;
	}
	if ((sess->xfer.desired_len == 0)
	    && (sess->xfer.bytes_recv < sess->xfer.trans_len)) {
		sess->xfer.r2t_flag = 0;
	}

	if (sess->xfer.bytes_recv < sess->xfer.trans_len) {
		/* continue transfer */
		sess->readst = srs_data_hdr;
	} else {
		RETURN_NOT_EQUAL("Final bit", data.final, 1, TTD_CLEANUP, -1);
	}

	return 0;

}

int target_transfer_data(struct target_session *sess,
			 struct iscsi_scsi_cmd_args *args, struct iovec *sg,
			 int sg_len)
{
	struct iovec   *iov, *iov_ptr = NULL;
	int             iov_len;

	memset(&sess->xfer, 0, sizeof(sess->xfer));

	if ((!sess->sess_params.immediate_data) && args->length) {
		iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
			    "Cannot accept any Immediate data\n");
		args->status = SCSI_CHECK_CONDITION;
		return -1;
	}
	/* Make a copy of the iovec */

	if ((iov_ptr = malloc(sizeof(struct iovec) * sg_len)) == NULL) {
		iscsi_trace_error(__FILE__, __LINE__, "malloc() failed\n");
		return -1;
	}
	iov = iov_ptr;
	memcpy(iov, sg, sizeof(struct iovec) * sg_len);
	iov_len = sg_len;

	/*
	 * Read any immediate data.
	 */

	if (sess->sess_params.immediate_data && args->length) {
		if (sess->sess_params.max_data_seg) {
			RETURN_GREATER("args->length", args->length,
				       sess->sess_params.max_data_seg,
				       TTD_CLEANUP, -1);
		}
		/* Modify iov to include just immediate data */

		if (modify_iov(&iov, &iov_len, 0, args->length) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "modify_iov() failed\n");
			TTD_CLEANUP;
			return -1;
		}
		iscsi_trace(TRACE_SCSI_DATA, __FILE__, __LINE__,
			    "reading %d bytes immediate write data\n",
			    args->length);

		/* FIXME - VERY wrong - just a placeholder */
		memcpy(iov[0].iov_base, sess->buff, args->length);
		iov[0].iov_len -= args->length;

		iscsi_trace(TRACE_SCSI_DATA, __FILE__, __LINE__,
			    "successfully read %d bytes immediate write data\n",
			    args->length);
		sess->xfer.bytes_recv += args->length;
	}
	/*
	 * Read iSCSI data PDUs
	 */

	if (sess->xfer.bytes_recv >= args->trans_len) {
		RETURN_NOT_EQUAL("Final bit", args->final, 1, TTD_CLEANUP, -1);
		goto out;
	}

	sess->xfer.tag = args->tag;
	sess->xfer.trans_len = args->trans_len;
	sess->xfer.desired_len = MIN(sess->sess_params.first_burst,
			       args->trans_len) - sess->xfer.bytes_recv;

	if (send_r2t(sess)) {
		args->status = SCSI_CHECK_CONDITION;
		TTD_CLEANUP;
		return -1;
	}

	/* initiate read of first data PDU */
	iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
		    "NET: queueing read of %u bytes\n",
		    ISCSI_HEADER_LEN);
	gnet_conn_readn(sess->conn, ISCSI_HEADER_LEN);
	sess->readst = srs_data_hdr;

out:
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "successfully transferred %u bytes write data\n",
		    args->trans_len);
	TTD_CLEANUP;
	return 0;
}

/********************
 * Public Functions *
 ********************/

int target_init(struct globals *gp, targv_t * tv, char *TargetName)
{
	int             i;

	NEWARRAY(struct target_session, g_session, gp->max_sessions,
		 "target_init", return -1);
	strlcpy(gp->targetname, TargetName, sizeof(gp->targetname));
	if (gp->state == TARGET_INITIALIZING || gp->state == TARGET_INITIALIZED) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "duplicate target initialization attempted\n");
		return -1;
	}
	gp->state = TARGET_INITIALIZING;
	gp->tv = tv;

	for (i = 0; i < tv->c; i++) {
		if ((g_session[i].d = device_init(gp, tv, &tv->v[i])) < 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "device_init() failed\n");
			return -1;
		}
	}

	gp->state = TARGET_INITIALIZED;

	printf("TARGET: TargetName is %s\n", gp->targetname);

	return 0;
}

int target_sess_cleanup(struct target_session *sess)
{
	/* Clean up */

	if (param_list_destroy(sess->params) != 0) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "param_list_destroy() failed\n");
		return -1;
	}

	/* Terminate connection */
	gnet_conn_unref(sess->conn);

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "session %d: ended\n", sess->id);

	/* Destroy session */
	free(sess);

	return 0;
}

int target_shutdown(struct globals *gp)
{
	struct target_session *sess;
	GList          *tmp;

	if ((gp->state == TARGET_SHUTTING_DOWN)
	    || (gp->state == TARGET_SHUT_DOWN)) {
		iscsi_trace_error(__FILE__, __LINE__,
				  "duplicate target shutdown attempted\n");
		return -1;
	}
	gp->state = TARGET_SHUTTING_DOWN;
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "shutting down target\n");

	tmp = session_list;
	while (tmp) {
		sess = tmp->data;

		/* Need to replace with a call to session_destroy() */

		if (device_shutdown(sess) != 0) {
			iscsi_trace_error(__FILE__, __LINE__,
					  "device_shutdown() failed\n");
			return -1;
		}

		target_sess_cleanup(sess);

		tmp = tmp->next;
	}

	g_list_free(session_list);
	session_list = NULL;

	/* listen socket is shutdown at layer above us */

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "target shutdown complete\n");
	gp->state = TARGET_SHUT_DOWN;

	return 0;
}

static void target_read_evt(struct target_session *sess, GConnEvent * evt)
{
	uint8_t        *buf;
	uint32_t        v, pad_len = 0;
	enum session_read_state readst = sess->readst;

	switch (readst) {
	case srs_basic_hdr:
	case srs_data_hdr:
		buf = (uint8_t *) evt->buffer;

		memcpy(sess->header, buf, ISCSI_HEADER_LEN);

		v = sess->ahs_len = buf[4];
		buf[4] = 0;

		sess->buff_len = ntohl(*((uint32_t *) (void *)(buf + 4)));
		v += sess->buff_len;

		if (v & 0x3)
			pad_len = 4 - (v & 0x3);

		if (!v) {	/* PDU is complete, nothing else to read */
			sess->buff = NULL;
			sess->ahs = NULL;

			sess->readst = srs_basic_hdr;

			if (readst == srs_data_hdr)
				target_data_pdu(sess);
			else
				execute_t(sess, sess->header);

			/* initiate read of next PDU */
			iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
			    	"NET: queueing read of %u bytes\n",
			    	ISCSI_HEADER_LEN);
			gnet_conn_readn(sess->conn, ISCSI_HEADER_LEN);
			break;
		}

		iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
			    "NET: queueing read of %u bytes\n",
			    v + pad_len);
		gnet_conn_readn(sess->conn, v + pad_len);
		if (sess->readst == srs_data_hdr)
			sess->readst = srs_data;
		else
			sess->readst = srs_ahs_data;
		break;

	case srs_ahs_data:
	case srs_data:
		if (sess->ahs_len)
			sess->ahs = (uint8_t *) evt->buffer;
		else
			sess->ahs = NULL;

		if (evt->length > sess->ahs_len)
			sess->buff = (uint8_t *) evt->buffer + sess->ahs_len;
		else
			sess->buff = NULL;

		sess->readst = srs_basic_hdr;

		if (readst == srs_data)
			target_data_pdu(sess);
		else
			execute_t(sess, sess->header);

		/* initiate read of next PDU */
		iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
			    "NET: queueing read of %u bytes\n",
			    ISCSI_HEADER_LEN);
		gnet_conn_readn(sess->conn, ISCSI_HEADER_LEN);
		break;

	default:
		break;
	}
}

#if 0
static const char *gnconn_str[] = 
{
	[GNET_CONN_ERROR] = "GNET_CONN_ERROR",
	[GNET_CONN_CONNECT] = "GNET_CONN_CONNECT",
	[GNET_CONN_CLOSE] = "GNET_CONN_CLOSE",
	[GNET_CONN_TIMEOUT] = "GNET_CONN_TIMEOUT",
	[GNET_CONN_READ] = "GNET_CONN_READ",
	[GNET_CONN_WRITE] = "GNET_CONN_WRITE",
	[GNET_CONN_READABLE] = "GNET_CONN_READABLE",
	[GNET_CONN_WRITABLE] = "GNET_CONN_WRITABLE",
};
#endif

static void target_tcp_event(GConn * conn, GConnEvent * evt, gpointer user_data)
{
	struct target_session *sess = user_data;

	switch (evt->type) {
	case GNET_CONN_READ:
		target_read_evt(sess, evt);
		break;

	case GNET_CONN_ERROR:
	case GNET_CONN_CONNECT:
	case GNET_CONN_CLOSE:
	case GNET_CONN_TIMEOUT:
	case GNET_CONN_WRITE:
	case GNET_CONN_READABLE:
	case GNET_CONN_WRITABLE:
		/* do nothing */

#if 0
		iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
			    "NET: event %s ignored\n",
			    gnconn_str[evt->type]);
#endif
		break;
	}
}

int target_accept(struct globals *gp, GConn * conn)
{
	struct target_session *sess;
	char            remote[1024];
	char            local[1024];
	GInetAddr      *addr;
	gchar          *addr_s;
	struct iscsi_parameter **l;

	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "listener thread started\n");

	iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
		    "create, bind, listen OK\n");

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return -1;

	sess->globals = gp;
	sess->conn = conn;

	if (gnet_inetaddr_is_ipv6(conn->inetaddr))
		sess->address_family = ISCSI_IPv6;
	else
		sess->address_family = ISCSI_IPv4;

	/* get ASCII version of local network address */

	addr = gnet_tcp_socket_get_local_inetaddr(conn->socket);
	addr_s = gnet_inetaddr_get_canonical_name(addr);

	strcpy(local, addr_s);

	free(addr_s);
	gnet_inetaddr_unref(addr);

	/* get ASCII version of remote network address */
	addr_s = gnet_inetaddr_get_canonical_name(conn->inetaddr);

	strcpy(remote, addr_s);

	free(addr_s);

	strlcpy(sess->initiator, remote, sizeof(sess->initiator));

	snprintf(gp->targetaddress, sizeof(gp->targetaddress), "%s:%u,1",
		 local, gp->port);
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "IPv4/6 cxn accepted (local IP %s, remote IP %s)\n",
		    local, sess->initiator);
	iscsi_trace(TRACE_ISCSI_DEBUG, __FILE__, __LINE__,
		    "TargetAddress = \"%s\"\n", gp->targetaddress);

	/*
	 * ISCSI_PARAM_TYPE_LIST format:        <type> <key> <dflt> <valid list values>
	 * ISCSI_PARAM_TYPE_BINARY format:      <type> <key> <dflt> <valid binary values>
	 * ISCSI_PARAM_TYPE_NUMERICAL format:   <type> <key> <dflt> <max>
	 * ISCSI_PARAM_TYPE_DECLARATIVE format: <type> <key> <dflt> ""
	 */

	sess->params = NULL;
	l = &sess->params;

	/* CHAP Parameters */
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_LIST, "AuthMethod", "CHAP",
		       "CHAP,None", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_LIST, "CHAP_A", "None", "5",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "CHAP_N", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "CHAP_R", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "CHAP_I", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "CHAP_C", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "TargetPortalGroupTag",
		       "1", "1", return -1);
	/* CHAP Parameters */
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_LIST, "HeaderDigest", "None", "None",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_LIST, "DataDigest", "None", "None",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL, "MaxConnections", "1",
		       "1", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "SendTargets", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "TargetName", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "InitiatorName", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "TargetAlias", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "InitiatorAlias", "",
		       "", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "TargetAddress", "", "",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_OR, "InitialR2T", "Yes",
		       "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_AND, "OFMarker", "No",
		       "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_AND, "IFMarker", "No",
		       "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL_Z, "OFMarkInt", "1",
		       "65536", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL_Z, "IFMarkInt", "1",
		       "65536", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_AND, "ImmediateData", "Yes",
		       "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL_Z,
		       "MaxRecvDataSegmentLength", "8192", "16777215",
		       return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL_Z, "MaxBurstLength",
		       "262144", "16777215", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL_Z, "FirstBurstLength",
		       "65536", "16777215", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL, "DefaultTime2Wait", "2",
		       "2", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL, "DefaultTime2Retain",
		       "20", "20", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL, "MaxOutstandingR2T", "1",
		       "1", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_OR, "DataPDUInOrder", "Yes",
		       "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_BINARY_OR, "DataSequenceInOrder",
		       "Yes", "Yes,No", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_NUMERICAL, "ErrorRecoveryLevel", "0",
		       "0", return -1);
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_DECLARATIVE, "SessionType", "Normal",
		       "Normal,Discovery", return -1);
	/*
	 * Auth Result is not in specs, we use this key to pass
	 * authentication result
	 */
	PARAM_LIST_ADD(l, ISCSI_PARAM_TYPE_LIST, "AuthResult", "No",
		       "Yes,No,Fail", return -1);

	/* Set remaining session parameters  */

	sess->UsePhaseCollapsedRead = ISCSI_USE_PHASE_COLLAPSED_READ_DFLT;

	/* Loop for commands */
	gnet_conn_set_callback(conn, target_tcp_event, sess);

	iscsi_trace(TRACE_NET_DEBUG, __FILE__, __LINE__,
		    "NET: queueing read of %u bytes\n",
		    ISCSI_HEADER_LEN);
	gnet_conn_readn(conn, ISCSI_HEADER_LEN);
	sess->readst = srs_basic_hdr;

	return 0;
}
