/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  * Evhtp specific HTTP/1 code complementing restconf_main_native.c
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

/* The clixon evhtp code can be compiled with or without threading support 
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <pwd.h>
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/resource.h>

#include <openssl/ssl.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#ifdef HAVE_LIBEVHTP
#include <event2/buffer.h> /* evbuffer */
#define EVHTP_DISABLE_REGEX
#define EVHTP_DISABLE_EVTHR

#include <evhtp/evhtp.h>

#endif /* HAVE_LIBEVHTP */

#ifdef HAVE_LIBNGHTTP2 /* To get restconf_native.h include files right */
#include <nghttp2/nghttp2.h>
#endif

/* restconf */
#include "restconf_lib.h"       /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"       /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"
#include "restconf_native.h"   /* Restconf-openssl mode specific headers*/
#ifdef HAVE_LIBEVHTP
#include "restconf_evhtp.h"    /* evhtp http/1 */
#endif

#ifdef HAVE_LIBEVHTP
static char*
evhtp_method2str(enum htp_method m)
{
    switch (m){
    case htp_method_GET:
	return "GET";
	break;
    case htp_method_HEAD:
	return "HEAD";
	break;
    case htp_method_POST:
	return "POST";
	break;
    case htp_method_PUT:
	return "PUT";
	break;
    case htp_method_DELETE:
	return "DELETE";
	break;
    case htp_method_OPTIONS:
	return "OPTIONS";
	break;
    case htp_method_PATCH:
	return "PATCH";
	break;
#ifdef NOTUSED
    case htp_method_MKCOL:
	return "MKCOL";
	break;
    case htp_method_COPY:
	return "COPY";
	break;
    case htp_method_MOVE:
	return "MOVE";
	break;
    case htp_method_OPTIONS:
	return "OPTIONS";
	break;
    case htp_method_PROPFIND:
	return "PROPFIND";
	break;
    case htp_method_PROPPATCH:
	return "PROPPATCH";
	break;
    case htp_method_LOCK:
	return "LOCK";
	break;
    case htp_method_UNLOCK:
	return "UNLOCK";
	break;
    case htp_method_TRACE:
	return "TRACE";
	break;
    case htp_method_CONNECT:
	return "CONNECT";
	break;
#endif /* NOTUSED */
    default:
	return "UNKNOWN";
	break;
    }
}

static int
evhtp_print_header(evhtp_header_t *header,
		   void           *arg)
{
    clicon_debug(1, "%s %s %s", __FUNCTION__, header->key, header->val);
    return 0;
}

static int
evhtp_query_iterator(evhtp_header_t *hdr,
		     void           *arg)
{
    cvec   *qvec = (cvec *)arg;
    char   *key;
    char   *val;
    char   *valu = NULL;    /* unescaped value */
    cg_var *cv;

    key = hdr->key;
    val = hdr->val;
    if (uri_percent_decode(val, &valu) < 0)
	return -1;
    if ((cv = cvec_add(qvec, CGV_STRING)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_add");
	return -1;
    }
    cv_name_set(cv, key);
    cv_string_set(cv, valu);
    if (valu)
	free(valu); 
    return 0;
}

/*! Translate http header by capitalizing, prepend w HTTP_ and - -> _
 * Example: Host -> HTTP_HOST 
 */
static int
evhtp_convert_fcgi(evhtp_header_t *hdr,
		   void           *arg)
{
    clicon_handle h = (clicon_handle)arg;

    return restconf_convert_hdr(h, hdr->key, hdr->val);
}

/*! convert parameters from evhtp to  fcgi-like parameters used by clixon
 *
 * While all these params come via one call in fcgi, the information must be taken from
 * several different places in evhtp 
 * @param[in]  h    Clicon handle
 * @param[in]  req  Evhtp request struct
 * @param[out] qvec Query parameters, ie the ?<id>=<val>&<id>=<val> stuff
 * @retval     1    OK continue
 * @retval     0    Fail, dont continue
 * @retval    -1    Error
 * The following parameters are set:
 * QUERY_STRING
 * REQUEST_METHOD
 * REQUEST_URI
 * HTTPS
 * HTTP_HOST
 * HTTP_ACCEPT
 * HTTP_CONTENT_TYPE
 * @note there may be more used by an application plugin
 */
static int
convert_evhtp_params2clixon(clicon_handle    h,
			    evhtp_request_t *req,
			    cvec            *qvec)
{
    int           retval = -1;
    htp_method    meth;
    evhtp_uri_t  *uri;
    evhtp_path_t *path;
    evhtp_ssl_t  *ssl = NULL;
    char         *subject = NULL;
    cvec         *cvv = NULL;
    char         *cn;
    cxobj        *xerr = NULL;
    int           pretty;

    if ((uri = req->uri) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No uri");
	goto done;
    }
    if ((path = uri->path) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No path");
	goto done;
    }
    meth = evhtp_request_get_method(req);

    /* QUERY_STRING in fcgi but go direct to the info instead of putting it in a string?
     * This is different from all else: Ie one could have re-created a string here but
     * that would mean double parsing,...
     */
    if (qvec && uri->query)
	if (evhtp_kvs_for_each(uri->query, evhtp_query_iterator, qvec) < 0){
	    clicon_err(OE_CFG, errno, "evhtp_kvs_for_each");
	    goto done;
	}
    if (restconf_param_set(h, "REQUEST_METHOD", evhtp_method2str(meth)) < 0)
	goto done;
    if (restconf_param_set(h, "REQUEST_URI", path->full) < 0)
	goto done;
    clicon_debug(1, "%s proto:%d", __FUNCTION__, req->proto);
    pretty = clicon_option_bool(h, "CLICON_RESTCONF_PRETTY");
    /* XXX: Any two http numbers seem accepted by evhtp, like 1.99, 99.3 as http/1.1*/
    if (req->proto != EVHTP_PROTO_10 &&
	req->proto != EVHTP_PROTO_11){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid HTTP version number") < 0)
	    goto done;
	/* Select json as default since content-type header may not be accessible yet */
	if (api_return_err0(h, req, xerr, pretty, YANG_DATA_JSON, 0) < 0)
	    goto done;
	goto fail;
    }
    clicon_debug(1, "%s conn->ssl:%d", __FUNCTION__, req->conn->ssl?1:0);
    /* Slightly awkward way of taking cert subject and CN and add it to restconf parameters
     * instead of accessing it directly */
    if ((ssl = req->conn->ssl) != NULL){
	if (restconf_param_set(h, "HTTPS", "https") < 0) /* some string or NULL */
	    goto done;
	/* SSL subject fields, eg CN (Common Name) , can add more here? */
	if (ssl_x509_name_oneline(req->conn->ssl, &subject) < 0)
	    goto done;
	if (subject != NULL) {
	    if (uri_str2cvec(subject, '/', '=', 1, &cvv) < 0)
		goto done;
	    if ((cn = cvec_find_str(cvv, "CN")) != NULL){
		if (restconf_param_set(h, "SSL_CN", cn) < 0) /* Can be used by callback */
		    goto done;
	    }
	}
    }

    /* Translate all http headers by capitalizing, prepend w HTTP_ and - -> _
     * Example: Host -> HTTP_HOST 
     */
    if (evhtp_headers_for_each(req->headers_in, evhtp_convert_fcgi, h) < 0)
	goto done;
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (subject)
	free(subject);
    if (xerr)
	xml_free(xerr);
    if (cvv)
	cvec_free(cvv);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! We got -1 back from lower layers, create a 500 Internal Server error 
 * Catch all on fatal error. This does not terminate the process but closes request 
 * stream 
 */
static int
evhtp_internal_error(evhtp_request_t *req)
{
    if (strlen(clicon_err_reason) &&
	req->buffer_out){
	evbuffer_add_printf(req->buffer_out, "%s", clicon_err_reason);
	evhtp_send_reply(req, EVHTP_RES_500);
    }
    clicon_err_reset();
    return 0;
}

/*! Send reply
 * @see htp__create_reply_
 */
static int
native_send_reply(restconf_conn        *rc,
		  restconf_stream_data *sd,
		  evhtp_request_t      *req)
{
    int                   retval = -1;

    cg_var               *cv;
    int                   minor;
    int                   major;

    switch (req->proto) {
    case EVHTP_PROTO_10:
	if (req->flags & EVHTP_REQ_FLAG_KEEPALIVE) {
	    /* protocol is HTTP/1.0 and clients wants to keep established */
	    if (restconf_reply_header(sd, "Connection", "keep-alive") < 0)
		goto done;
	}
	major = htparser_get_major(req->conn->parser); /* XXX Look origin */
	minor = htparser_get_minor(req->conn->parser);
	break;
    case EVHTP_PROTO_11:
	if (!(req->flags & EVHTP_REQ_FLAG_KEEPALIVE)) {
	    /* protocol is HTTP/1.1 but client wanted to close */
	    if (restconf_reply_header(sd, "Connection", "keep-alive") < 0)
		goto done;
	}
	major = htparser_get_major(req->conn->parser);
	minor = htparser_get_minor(req->conn->parser);
	break;
    default:
	/* this sometimes happens when a response is made but paused before
	 * the method has been parsed */
	major = 1;
	minor = 0;
	break;
    }
    cprintf(sd->sd_outp_buf, "HTTP/%u.%u %u %s\r\n",
	    major,
	    minor,
	    sd->sd_code,
	    restconf_code2reason(sd->sd_code));
    /* Loop over headers */
    cv = NULL;
    while ((cv = cvec_each(sd->sd_outp_hdrs, cv)) != NULL)
	cprintf(sd->sd_outp_buf, "%s: %s\r\n", cv_name_get(cv), cv_string_get(cv));
    cprintf(sd->sd_outp_buf, "\r\n");
    // cvec_reset(rc->rc_outp_hdrs); /* Is now done in restconf_connection but can be done here */
    retval = 0;
 done:
    return retval;
}

/*!
 */
static int
restconf_evhtp_reply(restconf_conn        *rc,
		     restconf_stream_data *sd,
		     evhtp_request_t      *req)
{
    int                   retval = -1;

    req->status = sd->sd_code;
    req->flags |= EVHTP_REQ_FLAG_FINISHED; /* Signal to evhtp to read next request */
    /* If body, add a content-length header 
     *    A server MUST NOT send a Content-Length header field in any response
     * with a status code of 1xx (Informational) or 204 (No Content).  A
     * server MUST NOT send a Content-Length header field in any 2xx
     * (Successful) response to a CONNECT request (Section 4.3.6 of
     * [RFC7231]).
     */
    if (sd->sd_code != 204 && sd->sd_code > 199)
	if (restconf_reply_header(sd, "Content-Length", "%zu", sd->sd_body_len) < 0)
	    goto done;	
    /* Create reply and write headers */
    if (native_send_reply(rc, sd, req) < 0)
	goto done;
    /* Write a body */
    if (sd->sd_body){
	cbuf_append_str(sd->sd_outp_buf, cbuf_get(sd->sd_body));
    }
    retval = 0;
 done:
    return retval;
}

#ifdef HAVE_LIBNGHTTP2
/*! Check http/1 UPGRADE to http/2
 * If upgrade headers are encountered AND http/2 is configured, then 
 * - add upgrade headers or signal error
 * - set http2 flag get settings to and signal to upper layer to do the actual transition.
 * @retval   -1   Error
 * @retval    0   Yes, upgrade dont proceed with request
 * @retval    1   No upgrade, proceed with request
 * @note currently upgrade header is checked always if nghttp2 is configured but may be a 
 *       runtime config option
 */
static int
evhtp_upgrade_http2(clicon_handle         h,
		    restconf_stream_data *sd)
{
    int    retval = -1;
    char  *str;
    char  *settings;
    cxobj *xerr = NULL;
	
    if ((str = restconf_param_get(h, "HTTP_UPGRADE")) != NULL &&
	clicon_option_bool(h, "CLICON_RESTCONF_HTTP2_PLAIN") == 1){
	/* Only accept "h2c" */
	if (strcmp(str, "h2c") != 0){
	    if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid upgrade token") < 0)
		goto done; 
	    if (api_return_err0(h, sd, xerr, 1, YANG_DATA_JSON, 0) < 0)
		goto done;
	    if (xerr)
		xml_free(xerr);
	}
	else {
	    if (restconf_reply_header(sd, "Connection", "Upgrade") < 0)
		goto done;
	    if (restconf_reply_header(sd, "Upgrade", "h2c") < 0)
		goto done;
	    if (restconf_reply_send(sd, 101, NULL, 0) < 0) /* Swithcing protocols */
		goto done;
	    /* Signal http/2 upgrade to http/2 to upper restconf_connection handling */
	    sd->sd_upgrade2 = 1;
	    if ((settings = restconf_param_get(h, "HTTP_HTTP2_Settings")) != NULL &&
		(sd->sd_settings2 = (uint8_t*)strdup(settings)) == NULL){
		clicon_err(OE_UNIX, errno, "strdup");
		goto done;
	    }
	}
	retval = 0; /* Yes, upgrade or error */
    }
    else
	retval = 1; /* No upgrade, proceed with request */
 done:
    return retval;
}
#endif /* HAVE_LIBNGHTTP2 */

/*! Callback for each incoming http request for path /
 *
 * This are all messages except /.well-known, Registered with evhtp_set_cb
 *
 * @param[in] req  evhtp http request structure defining the incoming message
 * @param[in] arg  cx_evhtp handle clixon specific fields
 * @retval    void
 * Discussion: problematic if fatal error -1 is returneod from clixon routines 
 * without actually terminating. Consider:
 * 1) sending some error? and/or
 * 2) terminating the process? 
 */
void
restconf_path_root(evhtp_request_t *req,
		   void            *arg)
{
    int                   retval = -1;
    clicon_handle         h;
    int                   ret;
    evhtp_connection_t   *evconn;
    restconf_conn        *rc;
    restconf_stream_data *sd;
    size_t                len;
    unsigned char        *buf;
    int                   keep_params = 0; /* set to 1 if dont delete params */

    clicon_debug(1, "------------");
    if ((h = (clicon_handle)arg) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "arg is NULL");
	evhtp_internal_error(req);
	goto done;
    }
    /* evhtp connect struct */
    if ((evconn = evhtp_request_get_connection(req)) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "evhtp_request_get_connection");
	evhtp_internal_error(req);
	goto done;
    }
    /* get clixon request connect pointer from generic evhtp application pointer */
    rc = evconn->arg;
    if ((sd = restconf_stream_find(rc, 0)) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "No stream_data");
	evhtp_internal_error(req);
	goto done;
    }
    sd->sd_req = req;
    sd->sd_proto = (req->proto == EVHTP_PROTO_10)?HTTP_10:HTTP_11;
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, evhtp_print_header, h);
    /* Query vector, ie the ?a=x&b=y stuff */
    if (sd->sd_qvec)
	cvec_free(sd->sd_qvec);
    if ((sd->sd_qvec = cvec_new(0)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	evhtp_internal_error(req);
	goto done;
    }
    /* Get indata
     */
    if ((len = evbuffer_get_length(req->buffer_in)) > 0){
	if ((buf = evbuffer_pullup(req->buffer_in, len)) == NULL){
	    clicon_err(OE_CFG, errno, "evbuffer_pullup");
	    goto done;
	}
	cbuf_reset(sd->sd_indata);
	/* Note the pullup may not be null-terminated */
	cbuf_append_buf(sd->sd_indata, buf, len);
    }
    /* Convert parameters from evhtp to  fcgi-like parameters used by clixon
     * ret = 0 means an error has already been sent
     */
    if ((ret = convert_evhtp_params2clixon(h, req, sd->sd_qvec)) < 0){
	evhtp_internal_error(req);
	goto done;
    }
    /* Check sanity of session, eg ssl client cert validation, may set rc_exit */
    if (restconf_connection_sanity(h, rc, sd) < 0)
	goto done;
    if (rc->rc_exit == 0){
#ifdef HAVE_LIBNGHTTP2
	if (ret == 1){
	    if ((ret = evhtp_upgrade_http2(h, sd)) < 0){
		evhtp_internal_error(req);
		goto done;
	    }
	    if (ret == 0)
		keep_params = 1;
	}
#endif
	if (ret == 1){
	    /* call generic function */
	    if (api_root_restconf(h, sd, sd->sd_qvec) < 0){
		evhtp_internal_error(req);
		goto done;
	    }
	}
    }
    /* Clear input request parameters from this request */
    if (!keep_params && restconf_param_del_all(h) < 0){
	evhtp_internal_error(req);
	goto done;
    }
    /* All parameters for sending a reply are here
     */
    if (sd->sd_code){
	if (restconf_evhtp_reply(rc, sd, req) < 0){
	    evhtp_internal_error(req);
	    goto done;
	}
    }
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return; /* void */
}

/*! /.well-known callback
 *
 * @param[in] req  evhtp http request structure defining the incoming message
 * @param[in] arg  cx_evhtp handle clixon specific fields
 * @retval    void
 */
void
restconf_path_wellknown(evhtp_request_t *req,
			void            *arg)
{
    int                   retval = -1;
    clicon_handle         h;
    int                   ret;
    evhtp_connection_t   *evconn;
    restconf_conn        *rc;
    restconf_stream_data *sd;
    int                   keep_params = 0; /* set to 1 if dont delete params */

    clicon_debug(1, "------------");
    if ((h = (clicon_handle)arg) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "arg is NULL");
	evhtp_internal_error(req);
	goto done;
    }
    /* evhtp connect struct */
    if ((evconn = evhtp_request_get_connection(req)) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "evhtp_request_get_connection");
	evhtp_internal_error(req);
	goto done;
    }
    /* get clixon request connect pointer from generic evhtp application pointer */
    rc = evconn->arg;
    if ((sd = restconf_stream_find(rc, 0)) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "No stream_data");
	evhtp_internal_error(req);
	goto done;
    }
    sd->sd_req = req;
    sd->sd_proto = (req->proto == EVHTP_PROTO_10)?HTTP_10:HTTP_11;
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, evhtp_print_header, h);
    /* Query vector, ie the ?a=x&b=y stuff */
    if ((sd->sd_qvec = cvec_new(0)) ==NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	evhtp_internal_error(req);
	goto done;
    }
    /* Convert parameters from evhtp to  fcgi-like parameters used by clixon
     * ret = 0 means an error has already been sent
     */
    if ((ret = convert_evhtp_params2clixon(h, req, sd->sd_qvec)) < 0){
	evhtp_internal_error(req);
	goto done;
    }
    /* Check sanity of session, eg ssl client cert validation, may set rc_exit */
    if (restconf_connection_sanity(h, rc, sd) < 0)
	goto done;
    if (rc->rc_exit == 0){
#ifdef HAVE_LIBNGHTTP2
	if (ret == 1){
	    if ((ret = evhtp_upgrade_http2(h, sd)) < 0){
		evhtp_internal_error(req);
		goto done;
	    }
	    if (ret == 0)
		keep_params = 1;
	}
#endif
	if (ret == 1){
	    /* call generic function */
	    if (api_well_known(h, sd) < 0){
		evhtp_internal_error(req);
		goto done;
	    }
	}
    }
    /* Clear input request parameters from this request */
    if (!keep_params && restconf_param_del_all(h) < 0){
	evhtp_internal_error(req);
	goto done;
    }
    /* All parameters for sending a reply are here
     */
    if (sd->sd_code){
	if (restconf_evhtp_reply(rc, sd, req) < 0){
	    evhtp_internal_error(req);
	    goto done;
	}
    }
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return; /* void */
}
#endif /* HAVE_LIBEVHTP */

