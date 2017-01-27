/*
 * Copyright (C) 2017 OpenSIPS Project
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * History:
 * ---------
 *  2017-01-24  created (razvanc)
 */

#include "../../mem/shm_mem.h"
#include "../../sr_module.h"
#include "../../db/db_id.h"
#include "../../lib/list.h"
#include "../../mod_fix.h"
#include "../../dprint.h"
#include "../../ut.h"

#include "rmq_servers.h"

#if defined AMQP_VERSION && AMQP_VERSION >= 0x00040000
  #define AMQP_VERSION_v04
#include <amqp_tcp_socket.h>
#endif

static LIST_HEAD(rmq_servers);

enum rmq_func_param_type { RMQT_SERVER, RMQT_PVAR };
struct rmq_func_param {
	enum rmq_func_param_type type;
	void *value;
};

/* function that checks for error */
static int rmq_error(char const *context, amqp_rpc_reply_t x)
{
	amqp_connection_close_t *mconn;
	amqp_channel_close_t *mchan;

	switch (x.reply_type) {
		case AMQP_RESPONSE_NORMAL:
			return 0;

		case AMQP_RESPONSE_NONE:
			LM_ERR("%s: missing RPC reply type!", context);
			break;

		case AMQP_RESPONSE_LIBRARY_EXCEPTION:
			LM_ERR("%s: %s\n", context,  "(end-of-stream)");
			break;

		case AMQP_RESPONSE_SERVER_EXCEPTION:
			switch (x.reply.id) {
				case AMQP_CONNECTION_CLOSE_METHOD:
					mconn = (amqp_connection_close_t *)x.reply.decoded;
					LM_ERR("%s: server connection error %d, message: %.*s",
							context, mconn->reply_code,
							(int)mconn->reply_text.len,
							(char *)mconn->reply_text.bytes);
					break;
				case AMQP_CHANNEL_CLOSE_METHOD:
						mchan = (amqp_channel_close_t *)x.reply.decoded;
					LM_ERR("%s: server channel error %d, message: %.*s",
							context, mchan->reply_code,
							(int)mchan->reply_text.len,
							(char *)mchan->reply_text.bytes);
					break;
				default:
					LM_ERR("%s: unknown server error, method id 0x%08X",
							context, x.reply.id);
					break;
			}
			break;
	}
	return -1;
}

/* function used to get a rmq_server based on a cid */
struct rmq_server *rmq_get_server(str *cid)
{
	struct list_head *it;
	struct rmq_server *srv;

	list_for_each(it, &rmq_servers) {
		srv = container_of(it, struct rmq_server, list);
		if (srv->cid.len == cid->len && memcmp(srv->cid.s, cid->s, cid->len) == 0)
			return srv;
	}
	return NULL;
}

struct rmq_server *rmq_resolve_server(struct sip_msg *msg, char *param)
{
	struct rmq_func_param *p = (struct rmq_func_param *)param;
	str cid;

	if (p->type == RMQT_SERVER)
		return p->value;

	if (fixup_get_svalue(msg, (gparam_p)param, &cid) < 0) {
		LM_ERR("cannot get the connection id!\n");
		return NULL;
	}
	return rmq_get_server(&cid);
}

static void rmq_close_server(struct rmq_server *srv)
{
	switch (srv->state) {
	case RMQS_ON:
	case RMQS_CONN:
		rmq_error("closing channel",
				amqp_channel_close(srv->conn, 1, AMQP_REPLY_SUCCESS));
	case RMQS_INIT:
		rmq_error("closing connection",
				amqp_connection_close(srv->conn, AMQP_REPLY_SUCCESS));
		if (amqp_destroy_connection(srv->conn) < 0)
			LM_ERR("cannot destroy connection\n");
	case RMQS_OFF:
		break;
	default:
		LM_WARN("Unknown rmq server state %d\n", srv->state);
	}
	srv->state = RMQS_OFF;
}

#if 0
static void rmq_destroy_server(struct rmq_server *srv)
{
	rmq_close_server(srv);
	if (srv->exchange.bytes)
		amqp_bytes_free(srv->exchange);
	pkg_free(srv);
}
#endif

/*
 * function used to reconnect a RabbitMQ server
 */
int rmq_reconnect(struct rmq_server *srv)
{
#if defined AMQP_VERSION_v04
	amqp_socket_t *amqp_sock;
#endif
	int socket;

	switch (srv->state) {
	case RMQS_OFF:
		srv->conn = amqp_new_connection();
		if (!srv) {
			LM_ERR("cannot create amqp connection!\n");
			return -1;
		}
#if defined AMQP_VERSION_v04
		amqp_sock = amqp_tcp_socket_new(srv->conn);
		if (!amqp_sock) {
			LM_ERR("cannot create AMQP socket\n");
			goto clean_rmq_conn;
		}
		socket = amqp_socket_open(amqp_sock, srv->uri.host, srv->uri.port);
		if (socket < 0) {
			LM_ERR("cannot open AMQP socket\n");
			goto clean_rmq_conn;
		}
#else
		socket = amqp_open_socket(srv->uri.host, srv->uri.port);
		if (socket < 0) {
			LM_ERR("cannot open AMQP socket\n");
			goto clean_rmq_conn;
		}
		amqp_set_sockfd(srv->conn, socket);
#endif
		srv->state = RMQS_INIT;
	case RMQS_INIT:
		if (rmq_error("Logging in", amqp_login(
				srv->conn,
				srv->uri.vhost,
				0,
				srv->max_frames,
				srv->heartbeat,
				AMQP_SASL_METHOD_PLAIN,
				srv->uri.user,
				srv->uri.password)))
			goto clean_rmq_server;
		/* all good - return success */
		srv->state = RMQS_CONN;
	case RMQS_CONN:
		/* don't use more than 1 channel */
		amqp_channel_open(srv->conn, 1);
		if (rmq_error("Opening channel", amqp_get_rpc_reply(srv->conn)))
			goto clean_rmq_server;
		LM_DBG("[%.*s] successfully connected!\n", srv->cid.len, srv->cid.s);
	case RMQS_ON:
		return 0;
	default:
		LM_WARN("Unknown rmq server state %d\n", srv->state);
		return -1;
	}
clean_rmq_server:
	rmq_close_server(srv);
	return -2;
clean_rmq_conn:
	if (amqp_destroy_connection(srv->conn) < 0)
		LM_ERR("cannot destroy connection\n");
	return -1;
}

#define IS_WS(_c) ((_c) == ' ' || (_c) == '\t' || (_c) == '\r' || (_c) == '\n')

/*
 * function used to add a RabbitMQ server
 */
int rmq_server_add(modparam_t type, void * val)
{
	struct rmq_server *srv;
	str param, s, cid;
	str suri = {0, 0};
	char uri_pending = 0;
	char *uri;
	unsigned flags;
	int retries;
	int max_frames = RMQ_DEFAULT_FRAMES;
	int heartbeat = RMQ_DEFAULT_HEARTBEAT;
	str exchange = {0, 0};
	enum rmq_parse_param { RMQP_NONE, RMQP_URI, RMQP_FRAME, RMQP_HBEAT, RMQP_IMM,
		RMQP_MAND, RMQP_EXCH, RMQP_RETRY, RMQP_NOPER } state;

	if (type != STR_PARAM) {
		LM_ERR("invalid parameter type %d\n", type);
		return -1;
	}
	s.s = (char *)val;
	s.len = strlen(s.s);

	for (; s.len > 0; s.s++, s.len--)
		if (!IS_WS(*s.s))
			break;
	if (s.len <= 0 || *s.s != '[') {
		LM_ERR("cannot find connection id start: %.*s\n", s.len, s.s);
		return -1;
	}
	cid.s = s.s + 1;
	for (s.s++, s.len--; s.len > 0; s.s++, s.len--)
		if (*s.s == ']')
			break;
	if (s.len <= 0 || *s.s != ']') {
		LM_ERR("cannot find connection id end: %.*s\n", s.len, s.s);
		return -1;
	}
	cid.len = s.s - cid.s;

	/* check if the server was already defined */
	if (rmq_get_server(&cid)) {
		LM_ERR("Connection ID %.*s already defined! Please use different "
				"names for different connections!\n", cid.len, cid.s);
		return -1;
	}
#define IF_IS_PARAM(_p, _s, _l) \
	do { \
		if (s.len >= (sizeof(_p) - 1) && strncasecmp(s.s, (_p), sizeof(_p) - 1) == 0) { \
			LM_DBG("[%.*s] found parameter %s\n", cid.len, cid.s, (_p)); \
			s.s += (sizeof(_p) - 1); \
			s.len -= (sizeof(_p) - 1); \
			state = _s; \
			goto _l; \
		} \
	} while (0)

	/* server not found - parse this one */
	for (s.s++, s.len--; s.len > 0; s.s++, s.len--) {
		if (IS_WS(*s.s))
			continue;
		param = s;
		state = RMQP_NONE;
		IF_IS_PARAM("uri", RMQP_URI, value);
		IF_IS_PARAM("frames", RMQP_FRAME, value);
		IF_IS_PARAM("retries", RMQP_RETRY, value);
		IF_IS_PARAM("exchange", RMQP_EXCH, value);
		IF_IS_PARAM("heartbeat", RMQP_HBEAT, value);
		IF_IS_PARAM("immediate", RMQP_IMM, no_value);
		IF_IS_PARAM("mandatory", RMQP_MAND, no_value);
		IF_IS_PARAM("non-persistent", RMQP_NOPER, no_value);

		/* there is no known parameter here */
		goto no_value;

#undef IF_IS_PARAM

value:
		/* found a valid parameter - if uri has started, it should be ended */
		if (uri_pending) {
			suri.len -= param.len;
			uri_pending = 0;
		}
		/* skip spaces before = */
		for (; s.len > 0; s.s++, s.len--)
			if (!IS_WS(*s.s))
				break;
		if (s.len <= 0 || *s.s != '=') {
			LM_ERR("[%.*s] cannot find uri equal: %.*s\n", cid.len, cid.s,
					param.len, param.s);
			return -1;
		}
		s.s++;
		s.len--;
		param = s; /* start of the parameter */

no_value:
		/* search for the next ';' */
		for (; s.len > 0; s.s++, s.len--)
			if (*s.s == ';')
				break;
		if (state != RMQP_URI)
			param.len -= s.len;
		trim_len(param.len, param.s, param);

		/* here is the end of parameter  - handle it */
		switch (state) {
		case RMQP_URI:
			/* remember where the uri starts */
			suri = param;
			uri_pending = 1;
			break;
		case RMQP_NONE:
			/* we eneded up in a place that has ';' - if we haven't found
			 * the end of the uri, this is also part of the uri. otherwise it
			 * is an error and we shall report it */
			if (!uri_pending) {
				LM_ERR("[%.*s] Unknown parameter: %.*s\n", cid.len, cid.s,
						param.len, param.s);
				return -1;
			}
			break;
		case RMQP_FRAME:
			if (str2int(&param, (unsigned int *)&max_frames) < 0) {
				LM_ERR("[%.*s] frames must be a number: %.*s\n",
						cid.len, cid.s, param.len, param.s);
				return -1;
			}
			if (max_frames < RMQ_MIN_FRAMES) {
				LM_WARN("[%.*s] number of frames is %d - less than expected %d! "
						"setting to expected\n", cid.len, cid.s, max_frames, RMQ_MIN_FRAMES);
				max_frames = RMQ_MIN_FRAMES;
			} else {
				LM_DBG("[%.*s] setting frames to %d\n", cid.len, cid.s, max_frames);
			}
			break;
		case RMQP_HBEAT:
			if (str2int(&param, (unsigned int *)&heartbeat) < 0) {
				LM_ERR("[%.*s] heartbeat must be the number of seconds, not %.*s\n",
						cid.len, cid.s, param.len, param.s);
				return -1;
			}
			if (heartbeat < 0) {
				LM_WARN("[%.*s] invalid number of heartbeat seconds %d! Using default!\n",
						cid.len, cid.s, heartbeat);
				heartbeat = RMQ_DEFAULT_HEARTBEAT;
			} else {
				LM_DBG("[%.*s] setting heartbeat to %d\n", cid.len, cid.s, heartbeat);
			}
			break;
		case RMQP_RETRY:
			if (str2int(&param, (unsigned int *)&retries) < 0) {
				LM_ERR("[%.*s] reties must be a number, not %.*s\n",
						cid.len, cid.s, param.len, param.s);
				return -1;
			}
			if (retries < 0) {
				LM_WARN("[%.*s] invalid number of retries %d! Using default!\n",
						cid.len, cid.s, retries);
				retries = RMQ_DEFAULT_RETRIES;
			} else {
				LM_DBG("[%.*s] %d number of retries in case of error\n",
						cid.len, cid.s, heartbeat);
			}
			break;
		case RMQP_IMM:
			flags &= RMQF_IMM;
			break;
		case RMQP_MAND:
			flags &= RMQF_MAND;
			break;
		case RMQP_NOPER:
			flags &= RMQF_NOPER;
			break;
		case RMQP_EXCH:
			exchange = param;
			LM_DBG("[%.*s] setting exchange '%.*s'\n", cid.len, cid.s,
					exchange.len, exchange.s);
			break;
		}
	}
	/* if we don't have an uri, we forfeit */
	if (!suri.s) {
		LM_ERR("[%.*s] cannot find an uri!", cid.len, cid.s);
		return -1;
	}
	/* trim the last spaces and ';' of the uri */
	trim_len(suri.len, suri.s, suri);
	if (suri.s[suri.len - 1] == ';')
		suri.len--;
	trim_len(suri.len, suri.s, suri);

	if ((srv = pkg_malloc(sizeof *srv + suri.len + 1)) == NULL) {
		LM_ERR("cannot alloc memory for rabbitmq server\n");
		return -1;
	}
	memset(srv, 0, sizeof *srv);
	uri = ((char *)srv) + sizeof *srv;
	memcpy(uri, suri.s, suri.len);
	uri[suri.len] = 0;

	if (amqp_parse_url(uri, &srv->uri) != 0) {
		LM_ERR("[%.*s] cannot parse rabbitmq uri: %s\n", cid.len, cid.s, uri);
		goto free;
	}

	if (srv->uri.ssl) {
		LM_WARN("[%.*s] we currently do not support ssl connections!\n", cid.len, cid.s);
		goto free;
	}

	if (exchange.len) {
		srv->exchange = amqp_bytes_malloc(exchange.len);
		if (!srv->exchange.bytes) {
			LM_ERR("[%.*s] cannot allocate echange buffer!\n", cid.len, cid.s);
			goto free;
		}
		memcpy(srv->exchange.bytes, exchange.s, exchange.len);
	} else
		srv->exchange = amqp_empty_bytes;

	srv->state = RMQS_OFF;
	srv->cid = cid;

	srv->flags = flags;
	srv->retries = retries;
	srv->max_frames = max_frames;
	srv->heartbeat = heartbeat;

	list_add(&srv->list, &rmq_servers);
	LM_DBG("[%.*s] new AMQP host=%s:%u\n", srv->cid.len, srv->cid.s,
			srv->uri.host, srv->uri.port);

	/* parse the url */
	return 0;
free:
	pkg_free(srv);
	return -1;
}
#undef IS_WS

/*
 * fixup function for rmq_server
 */
int fixup_rmq_server(void **param)
{
	str tmp;
	struct rmq_func_param *p;
	tmp.s = (char *)*param;
	tmp.len = strlen(tmp.s);
	trim_spaces_lr(tmp);
	if (tmp.len <= 0) {
		LM_ERR("invalid connection id!\n");
		return E_CFG;
	}
	p = pkg_malloc(sizeof(*p));
	if (!p) {
		LM_ERR("out of pkg memory!\n");
		return E_OUT_OF_MEM;
	}

	if (tmp.s[0] == PV_MARKER) {
		if (fixup_pvar(param) < 0) {
			LM_ERR("cannot parse cid\n");
			return E_UNSPEC;
		}
		p->value = *param;
		p->type = RMQT_PVAR;
	} else {
		p->value = rmq_get_server(&tmp);
		if (!p->value) {
			LM_ERR("unknown connection id=%.*s\n",
					tmp.len, tmp.s);
			return E_CFG;
		}
		p->type = RMQT_SERVER;
	}
	*param = p;
	return 0;
}

/*
 * function to connect all rmq servers
 */
void rmq_connect_servers(void)
{
	struct list_head *it;
	struct rmq_server *srv;

	list_for_each(it, &rmq_servers) {
		srv = container_of(it, struct rmq_server, list);
		if (rmq_reconnect(srv) < 0)
			LM_ERR("cannot connect to RabbitMQ server %s:%u\n",
					srv->uri.host, srv->uri.port);
	}
}
