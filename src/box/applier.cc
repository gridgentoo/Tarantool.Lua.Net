/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "applier.h"

#include <msgpuck.h>

#include "xlog.h"
#include "fiber.h"
#include "scoped_guard.h"
#include "coio.h"
#include "coio_buf.h"
#include "xstream.h"
#include "wal.h"
#include "xrow.h"
#include "replication.h"
#include "iproto_constants.h"
#include "version.h"
#include "trigger.h"
#include "xrow_io.h"
#include "error.h"
#include "session.h"

#include "cbus.h"
#include "schema.h"
#include "user.h"

#define APPLIER_IN_FLIGHT_LIMIT 1

extern struct cpipe tx_pipe;
extern struct cpipe net_pipe;

/* Count of in-flight appliers requests. */
static unsigned int appliers_in_flight = 0;
/* True if an applier requests queue should be flushed. */
static bool appliers_flush = false;
/* Wait condition for an appliers queue flush. */
static struct fiber_cond appliers_flush_cond;

/* TODO: add configuration options */
static const double RECONNECT_DELAY = 1.0;
static const double ACK_INTERVAL = 1.0;

STRS(applier_state, applier_STATE);

static inline void
applier_set_state(struct applier *applier, enum applier_state state)
{
	applier->state = state;
	say_debug("=> %s", applier_state_strs[state] +
		  strlen("APPLIER_"));
	trigger_run(&applier->on_state, applier);
}

/**
 * Write a nice error message to log file on SocketError or ClientError
 * in applier_f().
 */
static inline void
applier_log_error(struct applier *applier, struct error *e)
{
	uint32_t errcode = box_error_code(e);
	if (applier->last_logged_errcode == errcode)
		return;
	switch (applier->state) {
	case APPLIER_CONNECT:
		say_info("can't connect to master");
		break;
	case APPLIER_CONNECTED:
	case APPLIER_READY:
		say_info("can't join/subscribe");
		break;
	case APPLIER_AUTH:
		say_info("failed to authenticate");
		break;
	case APPLIER_FOLLOW:
	case APPLIER_INITIAL_JOIN:
	case APPLIER_FINAL_JOIN:
		say_info("can't read row");
		break;
	default:
		break;
	}
	error_log(e);
	if (type_cast(SocketError, e))
		say_info("will retry every %.2lf second", RECONNECT_DELAY);
	applier->last_logged_errcode = errcode;
}

struct applier_vclock_get_msg {
	struct cbus_call_msg base;
	struct vclock vclock;
};

/*
 * Copy replica vclock.
 */
static int
tx_applier_vclock_get(struct cbus_call_msg *m)
{
	struct applier_vclock_get_msg *msg = (struct applier_vclock_get_msg *)m;
	vclock_copy(&msg->vclock, &replicaset_vclock);
	return 0;
}

/*
 * Fiber function to write vclock to replication master.
 */
static int
applier_writer_f(va_list ap)
{
	struct applier *applier = va_arg(ap, struct applier *);
	struct ev_io io;
	coio_create(&io, applier->io.fd);

	/* Re-connect loop. */
	while (!fiber_is_cancelled()) {
		fiber_cond_wait_timeout(&applier->writer_cond, ACK_INTERVAL);
		/* Send ACKs only when in FINAL JOIN and FOLLOW modes */
		if (applier->state != APPLIER_FOLLOW &&
		    applier->state != APPLIER_FINAL_JOIN)
			continue;
		try {
			struct xrow_header xrow;
			struct applier_vclock_get_msg msg;
			/*
			 * Replica vclock are in main cord but applier
			 * writer fiber lives in net cord, so copy it
			 * over cbus call.
			 */
			bool cancellable = fiber_set_cancellable(false);
			int res = cbus_call(&tx_pipe, &net_pipe, &msg.base,
					    tx_applier_vclock_get, NULL,
					    TIMEOUT_INFINITY);
			fiber_set_cancellable(cancellable);
			if (res != 0)
				diag_raise();
			xrow_encode_vclock(&xrow, &msg.vclock);
			coio_write_xrow(&io, &xrow);
		} catch (SocketError *e) {
			/*
			 * Do not exit, if there is a network error,
			 * the reader fiber will reconnect for us
			 * and signal our cond afterwards.
			 */
			e->log();
		} catch (Exception *e) {
			/*
			 * Out of memory encoding the message, ignore
			 * and try again after an interval.
			 */
			e->log();
		}
		fiber_gc();
	}
	return 0;
}

/**
 * Connect to a remote host and authenticate the client.
 */
void
applier_connect(struct applier *applier)
{
	struct ev_io *coio = &applier->io;
	struct iobuf *iobuf = applier->iobuf[applier->input_index];
	if (coio->fd >= 0)
		return;
	char greetingbuf[IPROTO_GREETING_SIZE];

	struct uri *uri = &applier->uri;
	/*
	 * coio_connect() stores resolved address to \a &applier->addr
	 * on success. &applier->addr_len is a value-result argument which
	 * must be initialized to the size of associated buffer (addrstorage)
	 * before calling coio_connect(). Since coio_connect() performs
	 * DNS resolution under the hood it is theoretically possible that
	 * applier->addr_len will be different even for same uri.
	 */
	applier->addr_len = sizeof(applier->addrstorage);
	applier_set_state(applier, APPLIER_CONNECT);
	coio_connect(coio, uri, &applier->addr, &applier->addr_len);
	assert(coio->fd >= 0);
	coio_readn(coio, greetingbuf, IPROTO_GREETING_SIZE);
	applier->last_row_time = ev_monotonic_now(loop());

	/* Decode instance version and name from greeting */
	struct greeting greeting;
	if (greeting_decode(greetingbuf, &greeting) != 0)
		tnt_raise(LoggedError, ER_PROTOCOL, "Invalid greeting");

	if (strcmp(greeting.protocol, "Binary") != 0) {
		tnt_raise(LoggedError, ER_PROTOCOL,
			  "Unsupported protocol for replication");
	}

	/*
	 * Forbid changing UUID dynamically on connect because
	 * applier is registered by UUID in the replica set.
	 */
	if (!tt_uuid_is_nil(&applier->uuid) &&
	    !tt_uuid_is_equal(&applier->uuid, &greeting.uuid)) {
		Exception *e = tnt_error(ClientError, ER_INSTANCE_UUID_MISMATCH,
					 tt_uuid_str(&applier->uuid),
					 tt_uuid_str(&greeting.uuid));
		applier_log_error(applier, e);
		e->raise();
	}

	if (applier->version_id != greeting.version_id) {
		say_info("remote master is %u.%u.%u at %s\r\n",
			 version_id_major(greeting.version_id),
			 version_id_minor(greeting.version_id),
			 version_id_patch(greeting.version_id),
			 sio_strfaddr(&applier->addr, applier->addr_len));
	}

	/* Save the remote instance version and UUID on connect. */
	applier->uuid = greeting.uuid;
	applier->version_id = greeting.version_id;

	/* Don't display previous error messages in box.info.replication */
	diag_clear(&fiber()->diag);

	applier_set_state(applier, APPLIER_CONNECTED);

	/* Detect connection to itself */
	if (tt_uuid_is_equal(&applier->uuid, &INSTANCE_UUID))
		tnt_raise(ClientError, ER_CONNECTION_TO_SELF);

	/* Perform authentication if user provided at least login */
	if (!uri->login)
		goto done;

	/* Authenticate */
	applier_set_state(applier, APPLIER_AUTH);
	struct xrow_header row;
	xrow_encode_auth_xc(&row, greeting.salt, greeting.salt_len, uri->login,
			    uri->login_len, uri->password, uri->password_len);
	coio_write_xrow(coio, &row);
	coio_read_xrow(coio, &iobuf->in, &row);
	applier->last_row_time = ev_monotonic_now(loop());
	if (row.type != IPROTO_OK)
		xrow_decode_error_xc(&row); /* auth failed */

done:
	/* auth succeeded */
	say_info("authenticated");
	applier_set_state(applier, APPLIER_READY);

	if (applier->version_id >= version_id(1, 7, 4)) {
		/* Enable replication ACKs for newer servers */
		assert(applier->writer == NULL);

		char name[FIBER_NAME_MAX];
		int pos = snprintf(name, sizeof(name), "applierw/");
		uri_format(name + pos, sizeof(name) - pos, &applier->uri, false);

		applier->writer = fiber_new_xc(name, applier_writer_f);
		fiber_set_joinable(applier->writer, true);
		fiber_start(applier->writer, applier);
	}
}

struct applier_xrow_msg: public cmsg
{
	int len;
	struct iobuf *iobuf;
	struct applier *applier;
	struct xstream *stream;
	struct xrow_header row;
	struct request request;
	struct diag diag;
};

static void
tx_applier_xrow_process(struct cmsg *m)
{
	struct applier_xrow_msg *msg = (struct applier_xrow_msg *)m;
	if (msg->applier->drop_requests)
		return;

	if (msg->row.type == IPROTO_OK) {
		/* Setup vclock from replication master. */
		xrow_decode_vclock_xc(&msg->row, &replicaset_vclock);
		return;
	}

	if (msg->stream == msg->applier->subscribe_stream &&
	    vclock_get(&replicaset_vclock, msg->row.replica_id) >= msg->row.lsn) {
		/* Already applied row, nothing to do. */
		return;
	}

	fiber_set_session(fiber(), msg->applier->session);
	fiber_set_user(fiber(), &msg->applier->session->credentials);
	if (msg->stream == msg->applier->subscribe_stream) {
		/**
		 * Promote the replica set vclock before
		 * applying the row. If there is an
		 * exception (conflict) applying the row,
		 * the row is skipped when the replication
		 * is resumed.
		 */
		vclock_follow(&replicaset_vclock, msg->row.replica_id,
			      msg->row.lsn);
	}
	if (xstream_write(msg->stream, &msg->row) != 0) {
		/* Skip all subsequent requests. */
		msg->applier->drop_requests = true;
		diag_move(&fiber()->diag, &msg->diag);
	}
}

static void
net_applier_xrow_processed(struct cmsg *m)
{
	struct applier_xrow_msg *msg = (struct applier_xrow_msg *)m;
	msg->iobuf->in.rpos += msg->len;
	--appliers_in_flight;
	if (appliers_in_flight == 0) {
		/* There are no in-flight requests. */
		appliers_flush = false;
		fiber_cond_broadcast(&appliers_flush_cond);
	}
	if (ibuf_used(&msg->iobuf->in) == msg->applier->input_unparsed)
		fiber_cond_signal(&msg->applier->swap_buffers_cond);

	if (!diag_is_empty(&msg->diag)) {
		diag_move(&msg->diag, &msg->applier->cancel_reason);
		fiber_cancel(msg->applier->reader);
	}
	mempool_free(&msg->applier->msg_pool, msg);
}

static void
net_applier_xrow_send(struct applier *applier, struct xstream *stream,
		      struct xrow_header *row, int len)
{
	static const struct cmsg_hop apply_route[] = {
		{ tx_applier_xrow_process, &net_pipe },
		{ net_applier_xrow_processed, NULL },
	};
	struct applier_xrow_msg *msg = (struct applier_xrow_msg *)
		mempool_alloc_xc(&applier->msg_pool);
	cmsg_init(msg, apply_route);
	diag_create(&msg->diag);
	msg->len = len;
	msg->iobuf = applier->iobuf[applier->input_index];
	msg->applier = applier;
	msg->stream = stream;
	msg->row = *row;
	if (iproto_type_is_dml(row->type))
		xrow_decode_dml_xc(&msg->row, &msg->request,
				   dml_request_key_map(msg->row.type));

	while (appliers_flush) {
		/* Wait until all in-flight requests are done. */
		fiber_cond_wait(&appliers_flush_cond);
	}

	++appliers_in_flight;
	if ((msg->request.space_id <= BOX_SYSTEM_ID_MAX) ||
	    (appliers_in_flight >= APPLIER_IN_FLIGHT_LIMIT)) {
		/* In-flight count limit reached. */
		appliers_flush = true;
	}

	cpipe_push(&tx_pipe, msg);
}

/**
 * Return count of bytes to read to parse at least one xrow
 * or negative value if there is no free space to read.
 */
static ssize_t
applier_read_size(struct applier *applier)
{
	struct iobuf *cur_buf = applier->iobuf[applier->input_index];
	ssize_t to_read = 3;
	if (applier->input_unparsed) {
		const char *pos = cur_buf->in.wpos - applier->input_unparsed;
		if ((to_read = mp_check_uint(pos, cur_buf->in.wpos)) <= 0) {
			size_t len = mp_decode_uint(&pos);
			to_read = len - (cur_buf->in.wpos - pos);
		}
	}
	if (to_read < 0)
		return 0;

	if (to_read <= (ssize_t)ibuf_unused(&cur_buf->in))
		return to_read;

	if (ibuf_used(&cur_buf->in) == applier->input_unparsed) {
		ibuf_reserve_xc(&cur_buf->in, to_read);
		return to_read;
	}

	struct iobuf *new_buf = applier->iobuf[1 - applier->input_index];
	if (ibuf_used(&new_buf->in)) {
		/* Buffer is busy. */
		return -to_read;
	}

	/* Swap buffers. */
	iobuf_reset_mt(new_buf);
	ibuf_reserve_xc(&new_buf->in, to_read + applier->input_unparsed);
	memmove(new_buf->in.wpos, cur_buf->in.wpos - applier->input_unparsed,
		applier->input_unparsed);
	new_buf->in.wpos += applier->input_unparsed;
	cur_buf->in.wpos -= applier->input_unparsed;
	applier->input_index = 1 - applier->input_index;

	return to_read;
}

/**
 * Read data or yield if there is no space to read.
 */
static void
applier_read(struct applier *applier)
{
	ssize_t to_read;
	while ((to_read = applier_read_size(applier)) < 0)
		fiber_cond_wait(&applier->swap_buffers_cond);

	struct iobuf *iobuf = applier->iobuf[applier->input_index];
	struct ibuf *in = &iobuf->in;
	ssize_t readen = 0;
	if (to_read > 0) {
		readen = coio_readn_ahead(&applier->io, in->wpos,
					  to_read, ibuf_unused(in));
	}
	in->wpos += readen;
	applier->input_unparsed += readen;
}

/**
 * Parse xrow from input buffer, return xrow data length or
 * zero ifthere is not enought data.
 */
static size_t
applier_xrow_parse(struct applier *applier, struct xrow_header *row)
{
	if (applier->input_unparsed == 0)
		/* Nothing to parse. */
		return 0;

	struct iobuf *iobuf = applier->iobuf[applier->input_index];
	struct ibuf *in = &iobuf->in;

	const char *reqstart = in->wpos - applier->input_unparsed;
	const char *pos = reqstart;

	if (mp_typeof(*pos) != MP_UINT) {
		tnt_raise(ClientError, ER_INVALID_MSGPACK,
			  "packet length");
	}
	if (mp_check_uint(pos, in->wpos) >= 0)
		return 0;
	uint32_t len = mp_decode_uint(&pos);
	const char *reqend = pos + len;
	if (reqend > in->wpos)
		return 0;
	xrow_header_decode_xc(row, &pos, reqend);
	applier->input_unparsed -= reqend - reqstart;

	return reqend - reqstart;
}

/**
 * Execute and process JOIN request (bootstrap the instance).
 */
static void
applier_join(struct applier *applier)
{
	/* Send JOIN request */
	struct ev_io *coio = &applier->io;
	struct xrow_header row;
	xrow_encode_join_xc(&row, &INSTANCE_UUID);
	coio_write_xrow(coio, &row);

	/**
	 * Tarantool < 1.7.0: if JOIN is successful, there is no "OK"
	 * response, but a stream of rows from snapshot.
	 */
	if (applier->version_id >= version_id(1, 7, 0)) {
		/* Decode JOIN response */
		coio_read_xrow(coio, &applier->iobuf[applier->input_index]->in, &row);
		if (iproto_type_is_error(row.type)) {
			xrow_decode_error_xc(&row); /* re-throw error */
		} else if (row.type != IPROTO_OK) {
			tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
				  (uint32_t) row.type);
		}
		/*
		 * Start vclock. The vclock of the checkpoint
		 * the master is sending to the replica.
		 * Used to initialize the replica's initial
		 * vclock in bootstrap_from_master()
		 */
		net_applier_xrow_send(applier, NULL, &row, 0);
	}

	applier_set_state(applier, APPLIER_INITIAL_JOIN);

	/*
	 * Receive initial data.
	 */
	assert(applier->join_stream != NULL);
	while (true) {
		applier_read(applier);
		struct iobuf *iobuf = applier->iobuf[applier->input_index];
		size_t len;
		while ((len = applier_xrow_parse(applier, &row)) > 0) {
			applier->last_row_time = ev_now(loop());
			if (row.type == IPROTO_OK) {
				/* End of stream */
				if (applier->version_id < version_id(1, 7, 0)) {
					/*
					 * This is the start vclock if the
					 * server is 1.6. Since we have
					 * not initialized replication
					 * vclock yet, do it now. In 1.7+
					 * this vlcock is not used.
					 */
					net_applier_xrow_send(applier,
							      applier->join_stream,
							      &row, len);
				} else
					iobuf->in.rpos += len;
				goto initial_join_done;
			} else if (iproto_type_is_dml(row.type)) {
				net_applier_xrow_send(applier,
						      applier->join_stream,
						      &row, len);
			} else if (iproto_type_is_error(row.type)) {
				iobuf->in.rpos += len;
				xrow_decode_error_xc(&row);
			} else {
				tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
					  (uint32_t) row.type);
			}
		}
	}
initial_join_done:
	say_info("initial data received");

	applier_set_state(applier, APPLIER_FINAL_JOIN);

	/*
	 * Tarantool < 1.7.0: there is no "final join" stage.
	 */
	if (applier->version_id < version_id(1, 7, 0))
		goto finish;

	/*
	 * Receive final data.
	 */
	while (true) {
		applier_read(applier);
		struct iobuf *iobuf = applier->iobuf[applier->input_index];
		size_t len;
		while ((len = applier_xrow_parse(applier, &row)) > 0) {
			applier->last_row_time = ev_monotonic_now(loop());
			if (iproto_type_is_dml(row.type)) {
				net_applier_xrow_send(applier,
						      applier->subscribe_stream,
						      &row, len);
			} else if (row.type == IPROTO_OK) {
				/*
				 * Current vclock. This is not used now,
				 * ignore.
				 */
				iobuf->in.rpos += len;
				goto finish;
			} else if (iproto_type_is_error(row.type)) {
				iobuf->in.rpos += len;
				xrow_decode_error_xc(&row);  /* rethrow error */
			} else {
				tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
					  (uint32_t) row.type);
			}
		}
	}
finish:
	say_info("final data received");
	/* Wait until all pending requests will be done */
	while (appliers_in_flight)
		fiber_cond_wait(&appliers_flush_cond);

	applier_set_state(applier, APPLIER_JOINED);
	applier_set_state(applier, APPLIER_READY);
}

/**
 * Execute and process SUBSCRIBE request (follow updates from a master).
 */
static void
applier_subscribe(struct applier *applier)
{
	assert(applier->subscribe_stream != NULL);

	/* Send SUBSCRIBE request */
	struct ev_io *coio = &applier->io;
	struct iobuf *iobuf = applier->iobuf[applier->input_index];
	struct xrow_header row;

	xrow_encode_subscribe_xc(&row, &REPLICASET_UUID, &INSTANCE_UUID,
				 &replicaset_vclock);
	coio_write_xrow(coio, &row);
	applier_set_state(applier, APPLIER_FOLLOW);

	/*
	 * Read SUBSCRIBE response
	 */
	if (applier->version_id >= version_id(1, 6, 7)) {
		coio_read_xrow(coio, &iobuf->in, &row);
		if (iproto_type_is_error(row.type)) {
			xrow_decode_error_xc(&row);  /* error */
		} else if (row.type != IPROTO_OK) {
			tnt_raise(ClientError, ER_PROTOCOL,
				  "Invalid response to SUBSCRIBE");
		}
		/*
		 * In case of successful subscribe, the server
		 * responds with its current vclock.
		 */
		struct vclock vclock;
		vclock_create(&vclock);
		xrow_decode_vclock_xc(&row, &vclock);
	}
	/**
	 * Tarantool < 1.6.7:
	 * If there is an error in subscribe, it's sent directly
	 * in response to subscribe.  If subscribe is successful,
	 * there is no "OK" response, but a stream of rows from
	 * the binary log.
	 */

	/* Re-enable warnings after successful execution of SUBSCRIBE */
	applier->last_logged_errcode = 0;

	/*
	 * Process a stream of rows from the binary log.
	 */
	iobuf_reset(applier->iobuf[applier->input_index]);
	while (true) {
		applier_read(applier);
		struct iobuf *iobuf = applier->iobuf[applier->input_index];
		size_t len;
		while ((len = applier_xrow_parse(applier, &row)) > 0) {
			applier->lag = ev_now(loop()) - row.tm;
			applier->last_row_time = ev_monotonic_now(loop());

			if (iproto_type_is_error(row.type)) {
				iobuf->in.rpos += len;
				xrow_decode_error_xc(&row);  /* error */
			}
			/* Replication request. */
			if (row.replica_id == REPLICA_ID_NIL ||
			    row.replica_id >= VCLOCK_MAX) {
				/*
				 * A safety net, this can only occur
				 * if we're fed a strangely broken xlog.
				 */
				tnt_raise(ClientError, ER_UNKNOWN_REPLICA,
					  int2str(row.replica_id),
					  tt_uuid_str(&REPLICASET_UUID));
			}
			net_applier_xrow_send(applier,
					      applier->subscribe_stream,
					      &row, len);
		}
	}
}

static inline void
applier_disconnect(struct applier *applier, enum applier_state state)
{
	if (applier->writer != NULL) {
		fiber_cancel(applier->writer);
		applier->writer = NULL;
	}
	applier->drop_requests = true;
	/* Wait until all pending requests will be processed */
	if (appliers_in_flight) {
		fiber_cond_wait(&appliers_flush_cond);
		appliers_flush = true;
	}

	coio_close(loop(), &applier->io);
	iobuf_reset(applier->iobuf[0]);
	iobuf_reset(applier->iobuf[1]);
	applier_set_state(applier, state);
	fiber_gc();
}

static int
applier_f(va_list ap)
{
	struct applier *applier = va_arg(ap, struct applier *);
	/*
	 * Set correct session type for use in on_replace()
	 * triggers.
	 */
	current_session()->type = SESSION_TYPE_APPLIER;

	/* Re-connect loop */
	while (!fiber_is_cancelled()) {
		try {
			applier->drop_requests = false;
			applier_connect(applier);
			if (tt_uuid_is_nil(&REPLICASET_UUID)) {
				/*
				 * Execute JOIN if this is a bootstrap,
				 * and there is no snapshot. The
				 * join will pause the applier
				 * until WAL is created.
				 */
				applier_join(applier);
			}
			applier_subscribe(applier);
			/*
			 * subscribe() has an infinite loop which
			 * is stoppable only with fiber_cancel().
			 */
			unreachable();
			return 0;
		} catch (ClientError *e) {
			if (e->errcode() == ER_CONNECTION_TO_SELF &&
			    tt_uuid_is_equal(&applier->uuid, &INSTANCE_UUID)) {
				/* Connection to itself, stop applier */
				applier_disconnect(applier, APPLIER_OFF);
				return 0;
			} else if (e->errcode() == ER_LOADING) {
				/* Autobootstrap */
				applier_log_error(applier, e);
				goto reconnect;
			} else if (e->errcode() == ER_ACCESS_DENIED) {
				/* Invalid configuration */
				applier_log_error(applier, e);
				goto reconnect;
			} else if (e->errcode() == ER_SYSTEM) {
				/* System error from master instance. */
				applier_log_error(applier, e);
				goto reconnect;
			} else {
				/* Unrecoverable errors */
				applier_log_error(applier, e);
				applier_disconnect(applier, APPLIER_STOPPED);
				throw;
			}
		} catch (FiberIsCancelled *e) {
			if (!diag_is_empty(&applier->cancel_reason)) {
				applier_disconnect(applier, APPLIER_STOPPED);
				diag_move(&applier->cancel_reason, &fiber()->diag);
			} else {
				applier_disconnect(applier, APPLIER_OFF);
			}
			if (applier->fiber_nothrow)
				break;
			diag_raise();
		} catch (SocketError *e) {
			applier_log_error(applier, e);
			goto reconnect;
		}
		/* Put fiber_sleep() out of catch block.
		 *
		 * This is done to avoid the case when two or more
		 * fibers yield inside their try/catch blocks and
		 * throw an exception. Seems like the exception unwinder
		 * uses global state inside the catch block.
		 *
		 * This could lead to incorrect exception processing
		 * and crash the program.
		 *
		 * See: https://github.com/tarantool/tarantool/issues/136
		*/
reconnect:
		applier_disconnect(applier, APPLIER_DISCONNECTED);
		fiber_sleep(RECONNECT_DELAY);
	}
	return 0;
}

void
applier_start(struct applier *applier)
{
	char name[FIBER_NAME_MAX];
	assert(applier->reader == NULL);

	int pos = snprintf(name, sizeof(name), "applier/");
	uri_format(name + pos, sizeof(name) - pos, &applier->uri, false);

	struct fiber *f = fiber_new_xc(name, applier_f);
	/**
	 * So that we can safely grab the status of the
	 * fiber any time we want.
	 */
	fiber_set_joinable(f, true);
	applier->reader = f;
	fiber_start(f, applier);
}

static int
applier_free_msg(struct cbus_call_msg *m)
{
	free(m);
	return 0;
}


struct applier_stop_msg
{
	struct cbus_call_msg base;
	struct applier *applier;
};

static int
applier_do_stop(struct cbus_call_msg *m)
{
	struct applier_stop_msg *msg = (struct applier_stop_msg *)m;
	struct applier *applier = msg->applier;
	struct fiber *f = applier->reader;
	if (f == NULL)
		return 0;
	fiber_cancel(f);
	fiber_join(f);
	applier_set_state(applier, APPLIER_OFF);
	applier->reader = NULL;
	return 0;
}

void
tx_applier_stop(struct applier *applier)
{
	struct applier_stop_msg msg;
	msg.applier = applier;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&net_pipe, &tx_pipe, &msg.base, applier_do_stop, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

static void
applier_stop(struct applier *applier)
{
	struct applier_stop_msg msg;
	msg.applier = applier;
	(void) applier_do_stop(&msg.base);
}

struct applier_new_msg
{
	struct cbus_call_msg base;
	struct applier *applier;
	const char *uri;
	struct xstream *join_stream;
	struct xstream *subscribe_stream;
	struct session *session;
};

static int
applier_do_new(struct cbus_call_msg *m)
{
	struct applier_new_msg *msg = (struct applier_new_msg *)m;
	struct applier *applier = (struct applier *)
		calloc(1, sizeof(struct applier));
	if (applier == NULL) {
		diag_set(OutOfMemory, sizeof(*applier), "malloc",
			 "struct applier");
		return -1;
	}
	coio_create(&applier->io, -1);
	applier->iobuf[0] = iobuf_new();
	applier->iobuf[1] = iobuf_new();
	applier->input_index = 0;
	applier->input_unparsed = 0;
	fiber_cond_create(&applier->swap_buffers_cond);
	applier->session = msg->session;
	diag_create(&applier->cancel_reason);
	applier->fiber_nothrow = false;
	applier->drop_requests = false;
	mempool_create(&applier->msg_pool, &cord()->slabc,
		       sizeof(struct applier_xrow_msg));

	/* uri_parse() sets pointers to applier->source buffer */
	snprintf(applier->source, sizeof(applier->source), "%s", msg->uri);
	int rc = uri_parse(&applier->uri, applier->source);
	/* URI checked by box_check_replication() */
	assert(rc == 0 && applier->uri.service != NULL);
	(void) rc;

	applier->join_stream = msg->join_stream;
	applier->subscribe_stream = msg->subscribe_stream;
	applier->last_row_time = ev_monotonic_now(loop());
	rlist_create(&applier->on_state);
	fiber_channel_create(&applier->pause, 0);
	fiber_cond_create(&applier->writer_cond);
	msg->applier = applier;

	return 0;
}

struct applier *
applier_new(const char *uri, struct xstream *join_stream,
	    struct xstream *subscribe_stream)
{
	struct applier_new_msg msg;
	msg.applier = NULL;
	msg.uri = uri;
	msg.join_stream = join_stream;
	msg.subscribe_stream = subscribe_stream;
	msg.session = session_create(-1, SESSION_TYPE_APPLIER);
	credentials_init(&msg.session->credentials, admin_user->auth_token, admin_user->def->uid);
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&net_pipe, &tx_pipe, &msg.base, applier_do_new, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
	return msg.applier;
}

struct applier_delete_msg
{
	struct cbus_call_msg base;
	struct applier *applier;
};

static int
applier_do_delete(struct cbus_call_msg *m)
{
	struct applier_delete_msg *msg = (applier_delete_msg *)m;
	struct applier *applier = msg->applier;
	assert(applier->reader == NULL);
	iobuf_delete(applier->iobuf[0]);
	iobuf_delete(applier->iobuf[1]);
	assert(applier->io.fd == -1);
	fiber_channel_destroy(&applier->pause);
	trigger_destroy(&applier->on_state);
	mempool_destroy(&applier->msg_pool);
	free(applier);
	return 0;
}

void
tx_applier_delete(struct applier *applier)
{
	struct applier_delete_msg msg;
	msg.applier = applier;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&net_pipe, &tx_pipe, &msg.base, applier_do_delete, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
	session_destroy(applier->session);

}

struct applier_resume_msg
{
	struct cbus_call_msg base;
	struct applier *applier;
};

static int
applier_do_resume(struct cbus_call_msg *m)
{
	struct applier_resume_msg *msg = (struct applier_resume_msg *)m;
	struct applier *applier = msg->applier;
	assert(!fiber_is_dead(applier->reader));
	void *data = NULL;
	fiber_channel_put_xc(&applier->pause, data);
	return 0;
}

void
tx_applier_resume(struct applier *applier)
{
	struct applier_resume_msg msg;
	msg.applier = applier;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&net_pipe, &tx_pipe, &msg.base, applier_do_resume, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

static inline void
applier_pause(struct applier *applier)
{
	/* Sleep until applier_resume() wake us up */
	void *data;
	fiber_channel_get_xc(&applier->pause, &data);
}

struct applier_on_state {
	struct trigger base;
	struct applier *applier;
	enum applier_state desired_state;
	struct fiber_channel wakeup;
};

/** Used by applier_connect_all() */
static void
applier_on_state_f(struct trigger *trigger, void *event)
{
	(void) event;
	struct applier_on_state *on_state =
		container_of(trigger, struct applier_on_state, base);

	struct applier *applier = on_state->applier;

	if (applier->state != APPLIER_OFF &&
	    applier->state != APPLIER_STOPPED &&
	    applier->state != on_state->desired_state)
		return;

	/* Wake up waiter */
	fiber_channel_put_xc(&on_state->wakeup, applier);

	applier_pause(applier);
}

static inline void
applier_add_on_state(struct applier *applier,
		     struct applier_on_state *trigger,
		     enum applier_state desired_state)
{
	trigger_create(&trigger->base, applier_on_state_f, NULL, NULL);
	trigger->applier = applier;
	fiber_channel_create(&trigger->wakeup, 0);
	trigger->desired_state = desired_state;
	trigger_add(&applier->on_state, &trigger->base);
}

static inline void
applier_clear_on_state(struct applier_on_state *trigger)
{
	fiber_channel_destroy(&trigger->wakeup);
	trigger_clear(&trigger->base);
}

static inline int
applier_wait_for_state(struct applier_on_state *trigger, double timeout)
{
	void *data = NULL;
	struct applier *applier = trigger->applier;
	applier->fiber_nothrow = true;

	if (fiber_channel_get_timeout(&trigger->wakeup, &data, timeout) != 0) {
		applier->fiber_nothrow = false;
		return -1; /* ER_TIMEOUT */
	}

	applier->fiber_nothrow = false;
	if (applier->state != trigger->desired_state) {
		assert(applier->state == APPLIER_OFF ||
		       applier->state == APPLIER_STOPPED);
		/* Re-throw the original error */
		assert(!diag_is_empty(&applier->reader->diag));
		diag_move(&applier->reader->diag, &fiber()->diag);
		return -1;
	}
	return 0;
}

struct applier_resume_to_state_msg
{
	struct cbus_call_msg base;
	struct applier *applier;
	enum applier_state state;
	double timeout;
};

static int
applier_do_resume_to_state(struct cbus_call_msg *m)
{
	struct applier_resume_to_state_msg *msg =
		(struct applier_resume_to_state_msg *)m;
	struct applier_on_state trigger;
	struct applier *applier = msg->applier;

	applier_add_on_state(applier, &trigger, msg->state);
	assert(!fiber_is_dead(applier->reader));
	void *data = NULL;
	fiber_channel_put_xc(&applier->pause, data);
	int rc = applier_wait_for_state(&trigger, msg->timeout);
	applier_clear_on_state(&trigger);
	if (rc != 0) {
		return 1;
	}
	assert(applier->state == msg->state);
	return 0;
}

void
tx_applier_resume_to_state(struct applier *applier, enum applier_state state,
			   double timeout)
{
	struct applier_resume_to_state_msg *msg =
		(struct applier_resume_to_state_msg *)malloc(sizeof(*msg));
	msg->applier = applier;
	msg->state = state;
	msg->timeout = timeout;
	int res = cbus_call(&net_pipe, &tx_pipe, &msg->base, applier_do_resume_to_state,
			    applier_free_msg, TIMEOUT_INFINITY);
	if (res >= 0)
		free(msg);
	if (res) {
		diag_raise();
	}
}

struct applier_connect_all_msg
{
	struct cbus_call_msg base;
	struct applier **appliers;
	int count;
	double timeout;
	struct recovery *recovery;
};

static int
applier_do_connect_all(struct cbus_call_msg *m)
{
	static bool appliers_init = true;
	if (appliers_init)
		fiber_cond_create(&appliers_flush_cond);
	appliers_init = false;
	/*
	 * Simultaneously connect to remote peers to receive their UUIDs
	 * and fill the resulting set:
	 *
	 * - create a single control channel;
	 * - register a trigger in each applier to wake up our
	 *   fiber via this channel when the remote peer becomes
	 *   connected and a UUID is received;
	 * - wait up to CONNECT_TIMEOUT seconds for `count` messages;
	 * - on timeout, raise a CFG error, cancel and destroy
	 *   the freshly created appliers (done in a guard);
	 * - an success, unregister the trigger, check the UUID set
	 *   for duplicates, fill the result set, return.
	 */
	struct applier_connect_all_msg *msg =
		(struct applier_connect_all_msg *)m;
 
	/* A channel from applier's on_state trigger is used to wake us up */
	IpcChannelGuard wakeup(msg->count);
	/* Memory for on_state triggers registered in appliers */
	struct applier_on_state triggers[VCLOCK_MAX];
	/* Wait results until this time */
	double deadline = ev_monotonic_now(loop()) + msg->timeout;

	/* Add triggers and start simulations connection to remote peers */
	for (int i = 0; i < msg->count; i++) {
		/* Register a trigger to wake us up when peer is connected */
		applier_add_on_state(msg->appliers[i], &triggers[i],
				     APPLIER_CONNECTED);
		/* Start background connection */
		applier_start(msg->appliers[i]);
	}

	/* Wait for all appliers */
	for (int i = 0; i < msg->count; i++) {
		double wait = deadline - ev_monotonic_now(loop());
		if (wait < 0.0 ||
		    applier_wait_for_state(&triggers[i], wait) != 0) {
			goto error;
		}
	}

	for (int i = 0; i < msg->count; i++) {
		assert(msg->appliers[i]->state == APPLIER_CONNECTED);
		/* Unregister the temporary trigger used to wake us up */
		applier_clear_on_state(&triggers[i]);
	}

	/* Now all the appliers are connected, finish. */
	return 0;
error:
	/* Destroy appliers */
	for (int i = 0; i < msg->count; i++) {
		applier_clear_on_state(&triggers[i]);
		applier_stop(msg->appliers[i]);
	}

	/* ignore original error */
	tnt_raise(ClientError, ER_CFG, "replication",
		  "failed to connect to one or more replicas");
}

void
tx_applier_connect_all(struct applier **appliers, int count, double timeout)
{
	if (count == 0)
		return; /* nothing to do */
	struct applier_connect_all_msg msg;
	msg.appliers = appliers;
	msg.count = count;
	msg.timeout = timeout;
	bool cancellable = fiber_set_cancellable(false);
	int res = cbus_call(&net_pipe, &tx_pipe, &msg.base,
			    applier_do_connect_all, NULL, TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
	if (res)
		diag_raise();
}
