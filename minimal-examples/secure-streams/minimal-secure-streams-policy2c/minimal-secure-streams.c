/*
 * lws-minimal-secure-streams-policy2c
 *
 * Written in 2010-2020 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 *
 * This reads policy JSON on stdin and emits it as compileable
 * C structs.
 *
 * It's useful if your platform is too space-constrained for a
 * JSON policy and needs to build a static policy in C via
 * LWS_WITH_SECURE_STREAMS_STATIC_POLICY_ONLY... this way you can
 * still create and maintain the JSON policy but implement it directly
 * as C structs in your code.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <assert.h>

static int interrupted, bad = 1;


static void
sigint_handler(int sig)
{
	interrupted = 1;
}

struct aggstr {
	struct aggstr *next;

	const char *orig;
	size_t offset;
};

static struct aggstr *rbomap,	/* retry / backoff object map */
		     *trustmap, /* trust store map */
		     *certmap;	/* x.509 cert map */
static size_t last_offset;



static const char *
purify_csymbol(const char *in, char *temp, size_t templen)
{
	const char *otemp = temp;

	assert (strlen(in) < templen);

	while (*in) {
		if ((*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') ||
		    (*in >= '0' && *in <= '9'))
			*temp++ = *in;
		else
			*temp++ = '_';

		in++;
	}

	*temp = '\0';

	return otemp;
}

int main(int argc, const char **argv)
{
	const lws_ss_policy_t *pol, *lastpol = NULL;
	struct lws_context_creation_info info;
	size_t json_size = 0, est = 0;
	struct lws_context *context;
	const lws_ss_auth_t *auth;
	char prev[128], curr[128];
	int unique_rbo = 0, m, n;
	char buf[64], buf1[64];
	lws_ss_metadata_t *md;
	struct aggstr *a, *a1;

	signal(SIGINT, sigint_handler);

	memset(&info, 0, sizeof info);
	lws_cmdline_option_handle_builtin(argc, argv, &info);

	lwsl_user("LWS secure streams policy2c [-d<verb>]\n");

	info.fd_limit_per_thread = 1 + 6 + 1;
	info.port = CONTEXT_PORT_NO_LISTEN;

	info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
		       LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

	/* create the context */

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	lws_ss_policy_parse_begin(context, 0);

	printf("/*\n * Autogenerated from the following JSON policy\n */\n\n#if 0\n");

	do {
		int m, n = read(0, buf, sizeof(buf));

		if (n < 1)
			break;

		m = lws_ss_policy_parse(context, (uint8_t *)buf, (size_t)n);

		printf("%.*s", n, buf);
		json_size += n;

		if (m < 0 && m != LEJP_CONTINUE) {
			lwsl_err("%s: policy parse failed... lws has WITH_ROLEs"
				 "for what's in the JSON?\n", __func__);
			goto bail;
		}
	} while (1);

	printf("\n\n Original JSON size: %zu\n#endif\n\n", json_size);

	lwsl_notice("%s: parsed JSON\n", __func__);

	/*
	 * Well, this is fun, isn't it... we have parsed the JSON into in-memory
	 * policy objects, and it has set the context policy pointer to the head
	 * of those but has not set the new policy (which would free the x.509).
	 *
	 * We want to walk the streamtype list first discovering unique objects
	 * and strings referenced there and emitting them compactly as C data,
	 * and then second to emit the streamtype linked-list referring to those
	 * objects.
	 *
	 * For const strings, we aggregate them and avoid generating extra
	 * pointers by encoding the reference as &_lws_ss_staticpol_str[xxx]
	 * where xxx is the fixed offset in the aggregated monster-string.  When
	 * doing that, we keep a map of original pointers to offsets.
	 *
	 * Although we want to minimize memory used by the emitted C, we don't
	 * have to sweat memory during this conversion since it's happening on a
	 * PC
	 */

	pol = lws_ss_policy_get(context);

	while (pol) {

		/*
		 * Walk the metadata list gathering strings and issuing the
		 * C struct
		 */

		md = pol->metadata;

		if (md) {
			int idx = 0;

			printf("\nstatic const lws_ss_metadata_t ");

			prev[0] = '\0';
			md = pol->metadata;
			while (md) {

				est += sizeof(lws_ss_metadata_t);

				lws_snprintf(curr, sizeof(curr), "_md_%s_%s",
					purify_csymbol(pol->streamtype, buf,
						       sizeof(buf)),
					purify_csymbol(md->name, buf1,
						       sizeof(buf1)));

				printf("%s = {\n", curr);
				if (prev[0])
					printf("\t.next = (void *)&%s, \n", prev);

				printf("\t.name = \"%s\",\n", (const char *)md->name);
				if (md->value)
					printf("\t.value = (void *)\"%s\",\n", (const char *)md->value);

				printf("\t.length = %d,\n", idx++); // md->length);

				printf("}");
				if (md->next)
					printf(",\n");

				lws_strncpy(prev, curr, sizeof(prev));

				md = md->next;
			}

			printf(";\n\n");
		}

		/*
		 * Create unique retry policies... have we seen this guy?
		 */

		if (pol->retry_bo) {
			a = rbomap;
			while (a) {
				if (a->orig == (const char *)pol->retry_bo)
					break;

				a = a->next;
			}

			if (!a) {

				/* We haven't seen it before and need to create it */

				a = malloc(sizeof(*a));
				if (!a)
					goto bail;
				a->next = rbomap;
				a->offset = unique_rbo++;
				a->orig = (const char *)pol->retry_bo;
				rbomap = a;

				printf("static const uint32_t _rbo_bo_%zu[] = {\n",
					a->offset);
				for (n = 0; n < pol->retry_bo->retry_ms_table_count; n++)
					printf(" %u, ", (unsigned int)
					       pol->retry_bo->retry_ms_table[n]);

				est += sizeof(uint32_t) *
					pol->retry_bo->retry_ms_table_count;

				printf("\n};\nstatic const "
				       "lws_retry_bo_t _rbo_%zu = {\n", a->offset);

				printf("\t.retry_ms_table = _rbo_bo_%zu,\n",
					a->offset);
				printf("\t.retry_ms_table_count = %u,\n",
					pol->retry_bo->retry_ms_table_count);
				printf("\t.conceal_count = %u,\n",
					pol->retry_bo->conceal_count);
				printf("\t.secs_since_valid_ping = %u,\n",
					pol->retry_bo->secs_since_valid_ping);
				printf("\t.secs_since_valid_hangup = %u,\n",
					pol->retry_bo->secs_since_valid_hangup);
				printf("\t.jitter_percent = %u,\n",
					pol->retry_bo->jitter_percent);
				printf("};\n");

				est += sizeof(lws_retry_bo_t);
			}
		}

		/*
		 * How about his trust store, it's new to us?
		 */

		if (pol->trust.store) {
			a = trustmap;
			while (a) {
				if (a->orig == (const char *)pol->trust.store)
					break;

				a = a->next;
			}

			if (!a) {

				/* it's new to us... */

				a = malloc(sizeof(*a));
				if (!a)
					goto bail;
				a->next = trustmap;
				a->offset = 0; /* don't care, just track seen */
				a->orig = (const char *)pol->trust.store;
				trustmap = a;

				/*
				 * Have a look through his x.509 stack...
				 * any that're new to us?
				 */

				for (n = 0; n < pol->trust.store->count; n++) {
					if (!pol->trust.store->ssx509[n])
						continue;
					a1 = certmap;
					while (a1) {
						if (a1->orig == (const char *)pol->trust.store->ssx509[n])
							break;
						a1 = a1->next;
					}

					if (!a1) {
						/*
						 * This x.509 cert is new to us...
						 * let's capture the DER
						 */

						a1 = malloc(sizeof(*a1));
						if (!a1)
							goto bail;
						a1->next = certmap;
						a1->offset = 0; /* don't care, just track seen */
						a1->orig = (const char *)pol->trust.store->ssx509[n];
						certmap = a1;

						printf("static const uint8_t _ss_der_%s[] = {\n",
							purify_csymbol(pol->trust.store->ssx509[n]->vhost_name,
									buf, sizeof(buf)));

						for (m = 0; m < (int)pol->trust.store->ssx509[n]->ca_der_len; m++) {
							if ((m & 7) == 0)
								printf("\t/* 0x%3x */ ", m);

							printf("0x%02X, ", pol->trust.store->ssx509[n]->ca_der[m]);
							if ((m & 7) == 7)
								printf("\n");
						}

						printf("\n};\nstatic const lws_ss_x509_t _ss_x509_%s = {\n",
								purify_csymbol(pol->trust.store->ssx509[n]->vhost_name,
								buf, sizeof(buf)));
						printf("\t.vhost_name = \"%s\",\n", pol->trust.store->ssx509[n]->vhost_name);
						printf("\t.ca_der = _ss_der_%s,\n",
							purify_csymbol(pol->trust.store->ssx509[n]->vhost_name,
								buf, sizeof(buf)));
						printf("\t.ca_der_len = %zu,\n", pol->trust.store->ssx509[n]->ca_der_len);
						printf("};\n");

						est += sizeof(lws_ss_x509_t) + pol->trust.store->ssx509[n]->ca_der_len;
					}

				}


				printf("static const lws_ss_trust_store_t _ss_ts_%s = {\n",
					purify_csymbol(pol->trust.store->name,
							buf, sizeof(buf)));

				printf("\t.name = \"%s\",\n", pol->trust.store->name);
				printf("\t.ssx509 = {\n");

				for (n = pol->trust.store->count - 1; n >= 0 ; n--)
					printf("\t\t&_ss_x509_%s,\n",
						pol->trust.store->ssx509[n]->vhost_name);

				printf("\t}\n};\n");

				est += sizeof(lws_ss_trust_store_t);

			}
		}

		pol = pol->next;
	}

	/*
	 * The auth map
	 */

	auth = lws_ss_auth_get(context);
	if (auth)
		printf("\nstatic const lws_ss_auth_t ");
	prev[0] = '\0';

	while (auth) {
		lws_snprintf(curr, sizeof(curr), "_ssau_%s",
			purify_csymbol(auth->name, buf, sizeof(buf)));

		printf("%s = {\n", curr);
		if (prev[0])
			printf("\t.next = (void *)&%s,\n", prev);

		printf("\t.name = \"%s\",\n", auth->name);
		printf("\t.streamtype = \"%s\",\n", auth->streamtype);
		printf("\t.blob = %d,\n", auth->blob_index);
		printf("}");
		if (auth->next)
			printf(",");
		else
			printf(";");
		printf("\n");

		lws_strncpy(prev, curr, sizeof(prev));

		auth = auth->next;
	}

	if (lws_ss_auth_get(context))
		printf("\n");


	/*
	 * The streamtypes
	 */

	pol = lws_ss_policy_get(context);

	printf("\nstatic const lws_ss_policy_t ");
	prev[0] = '\0';

	while (pol) {

		est += sizeof(*pol);

		lws_snprintf(curr, sizeof(curr), "_ssp_%s",
			purify_csymbol(pol->streamtype, buf, sizeof(buf)));
		printf("%s = {\n", curr);


		if (prev[0])
			printf("\t.next = (void *)&%s,\n", prev);

		printf("\t.streamtype = \"%s\",\n", pol->streamtype);
		if (pol->endpoint)
			printf("\t.endpoint = \"%s\",\n", pol->endpoint);
		if (pol->rideshare_streamtype)
			printf("\t.rideshare_streamtype = \"%s\",\n",
				pol->rideshare_streamtype);
		if (pol->payload_fmt)
			printf("\t.payload_fmt = \"%s\",\n",
				pol->payload_fmt);
		if (pol->socks5_proxy)
			printf("\t.socks5_proxy = \"%s\",\n",
				pol->socks5_proxy);

		if (pol->auth)
			printf("\t.auth = &_ssau_%s,\n",
			       purify_csymbol(pol->auth->name, buf, sizeof(buf)));

		{
			lws_ss_metadata_t *nv = pol->metadata, *last = NULL;

			while (nv) {
				last = nv;
				nv = nv->next;
			}
			if (pol->metadata)
				printf("\t.metadata = (void *)&_md_%s_%s,\n",
					purify_csymbol(pol->streamtype, buf, sizeof(buf)),
					purify_csymbol(last->name, buf1, sizeof(buf1)));
		}


		switch (pol->protocol) {
		case LWSSSP_H1:
		case LWSSSP_H2:
		case LWSSSP_WS:

			printf("\t.u = {\n\t\t.http = {\n");

			if (pol->u.http.method)
				printf("\t\t\t.method = \"%s\",\n",
					pol->u.http.method);
			if (pol->u.http.url)
				printf("\t\t\t.url = \"%s\",\n",
					pol->u.http.url);
			if (pol->u.http.multipart_name)
				printf("\t\t\t.multipart_name = \"%s\",\n",
					pol->u.http.multipart_name);
			if (pol->u.http.multipart_filename)
				printf("\t\t\t.multipart_filename = \"%s\",\n",
					pol->u.http.multipart_filename);
			if (pol->u.http.multipart_content_type)
				printf("\t\t\t.multipart_content_type = \"%s\",\n",
					pol->u.http.multipart_content_type);
			if (pol->u.http.auth_preamble)
				printf("\t\t\t.auth_preamble = \"%s\",\n",
					pol->u.http.auth_preamble);

			if (pol->u.http.blob_header[0]) {
				printf("\t\t\t.blob_header = {\n");
				for (n = 0; n < (int)LWS_ARRAY_SIZE(pol->u.http.blob_header); n++)
					if (pol->u.http.blob_header[n])
						printf("\t\t\t\t\"%s\",\n",
							pol->u.http.blob_header[n]);

				printf("\t\t\t},\n");
			}

			if (pol->protocol == LWSSSP_WS) {
				printf("\t\t\t.u = {\n\t\t\t\t.ws = {\n");
				if (pol->u.http.u.ws.subprotocol)
					printf("\t\t\t\t\t.subprotocol = \"%s\",\n",
						pol->u.http.u.ws.subprotocol);
				printf("\t\t\t\t\t.binary = %u\n", pol->u.http.u.ws.binary);
				printf("\t\t\t\t}\n\t\t\t},\n");
			}

			if (pol->u.http.resp_expect)
				printf("\t\t\t.resp_expect = %u,\n", pol->u.http.resp_expect);
			if (pol->u.http.fail_redirect)
				printf("\t\t\t.fail_redirect = %u,\n", pol->u.http.fail_redirect);

			printf("\t\t}\n\t},\n");

			break;
		case LWSSSP_MQTT:

			printf("\t.u = {\n\t\t.mqtt = {\n");

			if (pol->u.mqtt.topic)
				printf("\t\t\t.topic = \"%s\",\n",
					pol->u.mqtt.topic);
			if (pol->u.mqtt.subscribe)
				printf("\t\t\t.subscribe = \"%s\",\n",
					pol->u.mqtt.subscribe);
			if (pol->u.mqtt.will_topic)
				printf("\t\t\t.will_topic = \"%s\",\n",
					pol->u.mqtt.will_topic);
			if (pol->u.mqtt.will_message)
				printf("\t\t\t.will_message = \"%s\",\n",
					pol->u.mqtt.will_message);

			if (pol->u.mqtt.keep_alive)
				printf("\t\t\t.keep_alive = %u,\n",
					pol->u.mqtt.keep_alive);
			if (pol->u.mqtt.qos)
				printf("\t\t\t.qos = %u,\n",
					pol->u.mqtt.qos);
			if (pol->u.mqtt.clean_start)
				printf("\t\t\t.clean_start = %u,\n",
					pol->u.mqtt.clean_start);
			if (pol->u.mqtt.will_qos)
				printf("\t\t\t.will_qos = %u,\n",
					pol->u.mqtt.will_qos);
			if (pol->u.mqtt.will_retain)
				printf("\t\t\t.will_retain = %u,\n",
					pol->u.mqtt.will_retain);

			printf("\t\t}\n\t},\n");

			break;
		default:
			lwsl_err("%s: unknown ss protocol index %d\n", __func__,
					pol->protocol);
			goto bail;
		}

#if 0
		const lws_ss_trust_store_t *trust_store; /**< CA certs needed for conn
		       validation, only set between policy parsing and vhost creation */
#endif

		if (pol->retry_bo) {
			a = rbomap;
			while (a) {
				if (a->orig == (const char *)pol->retry_bo)
					break;

				a = a->next;
			}
			if (!a)
				goto bail;

			printf("\t.retry_bo = &_rbo_%zu,\n", a->offset);
		}

		if (pol->timeout_ms)
			printf("\t.timeout_ms = %u,\n", pol->timeout_ms);
		if (pol->flags)
			printf("\t.flags = 0x%x,\n", pol->flags);
		if (pol->port)
			printf("\t.port = %u,\n", pol->port);
		if (pol->metadata_count)
			printf("\t.metadata_count = %u,\n", pol->metadata_count);
		printf("\t.protocol = %u,\n", pol->protocol);
		if (pol->client_cert)
			printf("\t.client_cert = %u,\n", pol->client_cert);

		if (pol->trust.store)
			printf("\t.trust = {.store = &_ss_ts_%s},\n",
				purify_csymbol(pol->trust.store->name,
							buf, sizeof(buf)));


		printf("}");
		if (pol->next)
			printf(",\n");

		lws_strncpy(prev, curr, sizeof(prev));

		lastpol = pol;

		pol = pol->next;
	}

	printf(";\n");
	if (lastpol)
		printf("#define _ss_static_policy_entry _ssp_%s\n",
			purify_csymbol(lastpol->streamtype, buf, sizeof(buf)));

	est += last_offset;

	printf("/* estimated footprint %zu (when sizeof void * = %zu) */\n",
			est, sizeof(void *));

	lws_ss_policy_parse_abandon(context);
	bad = 0;

bail:


	lws_context_destroy(context);

	lwsl_user("Completed: %s\n", bad ? "failed" : "OK");

	return bad;
}
