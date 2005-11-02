/* Dynamic fetching of X.509 CRLs
 * Copyright (C) 2002 Stephane Laroche <stephane.laroche@colubris.com>
 * Copyright (C) 2000-2003 Andreas Steffen, Zuercher Hochschule Winterthur
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * RCSID $Id: fetch.c,v 1.2 2003/10/31 02:37:51 mcr Exp $
 */

#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <pthread.h>
#include <string.h>

#include <freeswan.h>

#ifdef LDAP_VER
#include <ldap.h>
#endif

#include "constants.h"
#include "defs.h"
#include "log.h"
#include "id.h"
#include "asn1.h"
#include "pem.h"
#include "x509.h"
#include "whack.h"
#include "fetch.h"

#define FETCH_CMD	"curl -B"
#define FETCH_CMD_TIMEOUT	4	/* seconds */

typedef struct fetch_req fetch_req_t;

struct fetch_req {
  fetch_req_t   *next;
  time_t        installed;
  int           trials;
  chunk_t       issuer;
  generalName_t *distributionPoints;
};

/* chained list of fetch requests */
static fetch_req_t *fetch_reqs  = NULL;

fetch_req_t empty_fetch_req = {
   NULL    , /* next */
         0 , /* installed */
         0 , /* trials */
  {NULL, 0}, /* issuer */
   NULL      /* distributionPoints */
};

static pthread_t thread;
static pthread_mutex_t cacert_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t crl_list_mutex =    PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fetch_list_mutex =  PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fetch_wake_mutex =  PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fetch_wake_cond =    PTHREAD_COND_INITIALIZER;

#define BUF_LEN		512

/*
 * lock access to the chained cacert list
 */
void
lock_cacert_list(const char *who)
{
    pthread_mutex_lock(&cacert_list_mutex);
    DBG(DBG_CONTROL,
	DBG_log("cacert list locked by '%s'", who)
    )
}

/*
 * unlock access to the chained cacert list
 */
void
unlock_cacert_list(const char *who)
{
    DBG(DBG_CONTROL,
	DBG_log("cacert list unlocked by '%s'", who)
    )
    pthread_mutex_unlock(&cacert_list_mutex);
}

/*
 * lock access to the chained crl list
 */
void
lock_crl_list(const char *who)
{
    pthread_mutex_lock(&crl_list_mutex);
    DBG(DBG_CONTROL,
	DBG_log("crl list locked by '%s'", who)
    )
}

/*
 * unlock access to the chained crl list
 */
void
unlock_crl_list(const char *who)
{
    DBG(DBG_CONTROL,
	DBG_log("crl list unlocked by '%s'", who)
    )
    pthread_mutex_unlock(&crl_list_mutex);
}

/*
 * lock access to the chained fetch request list
 */
static void
lock_fetch_list(const char *who)
{
    pthread_mutex_lock(&fetch_list_mutex);
    DBG(DBG_CONTROL,
	DBG_log("fetch request list locked by '%s'", who)
    )
}

/*
 * unlock access to the chained fetch request list
 */
static void
unlock_fetch_list(const char *who)
{
    DBG(DBG_CONTROL,
	DBG_log("fetch request list unlocked by '%s'", who)
    )
    pthread_mutex_unlock(&fetch_list_mutex);
}

/*
 * wakes up the sleeping fetch thread
 */
void
wake_fetch_thread(const char *who)
{
    if (crl_check_interval > 0)
    {
	DBG(DBG_CONTROL,
	    DBG_log("fetch thread wake call by '%s'", who)
	)
	pthread_mutex_lock(&fetch_wake_mutex);
	pthread_cond_signal(&fetch_wake_cond);
	pthread_mutex_unlock(&fetch_wake_mutex);
    }
}

/*
 *  free the dynamic memory used to store fetch requests
 */
static void
free_fetch_request(fetch_req_t *req)
{
    pfree(req->issuer.ptr);
    free_generalNames(req->distributionPoints, TRUE);
    pfree(req);
}

void
free_fetch_requests(void)
{
    lock_fetch_list("free_fetch_requests");

    while (fetch_reqs != NULL)
    {
	fetch_req_t *req = fetch_reqs;
	fetch_reqs = req->next;
	free_fetch_request(req);
    }

    unlock_fetch_list("free_fetch_requests");
}

/*
 * an url is not allowed to contain single quotes
 */
static err_t
clean_url(chunk_t url)
{
   if (memchr(url.ptr, '\'', url.len) != NULL)
	return "url contains single quotes";
   else
	return NULL;
}

/*
 * fetches a binary blob from a url via curl
 */
static err_t
fetch_curl(chunk_t url, chunk_t *blob)
{
    FILE *fp;
    fd_set rfds;
    size_t cmd_len;
    char *cmd;

    err_t ugh = clean_url(url);
    if (ugh != NULL)
	return ugh;

    cmd_len = sizeof(FETCH_CMD) + url.len + 2;
    cmd = alloc_bytes(cmd_len + 1, "curl cmd");
    sprintf(cmd, "%s '%.*s'", FETCH_CMD, (int)url.len, url.ptr);

    DBG(DBG_CONTROL,
	DBG_log("executing %s", cmd)
    )

    /* Call curl to extract crl */
    fp = popen(cmd, "r");
    if (!fp)
    {
	ugh = "Executing curl command failed";
    }
    else
    {
	/* read from pipe and put in memory */
	char *buf = NULL;
	int len = 0;
	int len2 = 0;
	int retval = 0;
	struct timeval timeout;

	do {
	    char *newbuf = realloc(buf, len+1024);
	    if (!newbuf)
	    {
		free(buf);
		break;
	    }

	    /* set timeout */
	    timeout.tv_sec = FETCH_CMD_TIMEOUT;
	    timeout.tv_usec = 0;

	    FD_ZERO(&rfds);
	    FD_SET(fileno(fp), &rfds);

	    retval = select(fileno(fp)+1, &rfds, NULL, NULL, &timeout);
	    if (retval > 0)
	    {
		buf = newbuf;
		len2 = fread(buf+len, 1, 1024, fp);
		len += len2;
	    }
	} while (retval > 0 && (len2 == 1024));

	if (len > 0)
	{
	    blob->len = len;
	    blob->ptr = alloc_bytes(len, "curl blob");
	    memcpy(blob->ptr, buf, len);
	}
	else
	{
	    ugh = "Zero size blob fetched";
	}
	free(buf);
    }

    /* cleanup */
    pclose(fp);
    pfree(cmd);

    return ugh;
}

#ifdef LDAP_VER
/*
 * parses the result returned by an ldap query
 */
static err_t
parse_ldap_result(LDAP * ldap, LDAPMessage *result, chunk_t *blob)
{
    err_t ugh = NULL;

    LDAPMessage * entry = ldap_first_entry(ldap, result);

    if (entry != NULL)
    {
	BerElement *ber = NULL;
	char *attr;
	
	attr = ldap_first_attribute(ldap, entry, &ber);

	if (attr != NULL)
	{
	    struct berval **values = ldap_get_values_len(ldap, entry, attr);

	    if (values != NULL)
	    {
		if (values[0] != NULL)
		{
		    blob->len = values[0]->bv_len;
		    blob->ptr = alloc_bytes(blob->len, "ldap blob");
		    memcpy(blob->ptr, values[0]->bv_val, blob->len);
		    if (values[1] != NULL)
		    {
			plog("warning: more than one value was fetched from LDAP URL");
		    }
		}
		else
		{
		    ugh = "no values in attribute";
		}
		ldap_value_free_len(values);
	    }
	    else
	    {
		ugh = ldap_err2string(ldap_result2error(ldap, entry, 0));
	    }
	    ldap_memfree(attr);
	}
	else
	{
	    ugh = ldap_err2string(ldap_result2error(ldap, entry, 0));
	}
	ber_free(ber, 0);
    }
    else
    {
	ugh = ldap_err2string(ldap_result2error(ldap, result, 0));
    }
    return ugh;
}

/*
 * fetches a binary blob from an ldap url
 */
static err_t
fetch_ldap_url(chunk_t url, chunk_t *blob)
{
    LDAPURLDesc *lurl;
    err_t ugh = NULL;
    int rc;

    char *ldap_url = alloc_bytes(url.len + 1, "ldap query");

    sprintf(ldap_url,"%.*s", (int)url.len, url.ptr);

    DBG(DBG_CONTROL,
	DBG_log("Trying LDAP URL '%s'", ldap_url)
    )

    rc = ldap_url_parse(ldap_url, &lurl);
    pfree(ldap_url);

    if (rc == LDAP_SUCCESS)
    {
	LDAP *ldap = ldap_init(lurl->lud_host, lurl->lud_port);

	if (ldap != NULL)
	{
	    int ldap_version = (LDAP_VER == 2)? LDAP_VERSION2 : LDAP_VERSION3;
	    struct timeval timeout;

	    timeout.tv_sec  = FETCH_CMD_TIMEOUT;
	    timeout.tv_usec = 0;
	    ldap_set_option(ldap, LDAP_OPT_PROTOCOL_VERSION, &ldap_version);
	    ldap_set_option(ldap, LDAP_OPT_NETWORK_TIMEOUT, &timeout);

	    rc = ldap_simple_bind_s(ldap, NULL, NULL);

	    if (rc == LDAP_SUCCESS)
	    {
		LDAPMessage *result;

		timeout.tv_sec = FETCH_CMD_TIMEOUT;
		timeout.tv_usec = 0;
		
		rc = ldap_search_st(ldap, lurl->lud_dn
					, lurl->lud_scope
					, lurl->lud_filter
					, lurl->lud_attrs
					, 0, &timeout, &result);

		if (rc == LDAP_SUCCESS)
		{
		    ugh = parse_ldap_result(ldap, result, blob);
		    ldap_msgfree(result);
		}
		else
		{
		    ugh = ldap_err2string(rc);
		}
	    }
	    else
	    {
		ugh = ldap_err2string(rc);
	    }
	    ldap_unbind_s(ldap);
	}
	else
	{
	    ugh = "ldap init";
	}
	ldap_free_urldesc(lurl);
    }
    else
    {
	ugh = ldap_err2string(rc);
    }
    return ugh;
}
#else
static err_t
fetch_ldap_url(chunk_t url UNUSED
	       , chunk_t *blob UNUSED)
{
    return "LDAP URL fetching not activated in pluto source code";
}
#endif

/*
 * fetch an ASN.1 blob coded in PEM or DER format from a URL
 */
static err_t
fetch_asn1_blob(chunk_t url, chunk_t *blob)
{
    err_t ugh = NULL;

    if (url.len >= 4 && strncasecmp(url.ptr, "ldap", 4) == 0)
    {
	ugh = fetch_ldap_url(url, blob);
    }
    else
    {
	ugh = fetch_curl(url, blob);
    }
    if (ugh != NULL)
	return ugh;

    if (is_asn1(*blob))
    {
	DBG(DBG_PARSING,
	    DBG_log("  fetched blob coded in DER format")
	)
    }
    else
    {
	bool pgp = FALSE;

	ugh = pemtobin(blob, NULL, "", &pgp);
	if (ugh == NULL)
	{
	    if (is_asn1(*blob))
	    {
		DBG(DBG_PARSING,
		    DBG_log("  fetched blob coded in PEM format")
		)
	    }
	    else
	    {
		ugh = "Blob coded in unknown format";
		pfree(blob->ptr);
	    }
	}
	else
	{
	    pfree(blob->ptr);
	}
    }
    return ugh;
}

/*
 * try to fetch the crls defined by the fetch requests
 */
static void
fetch_crls(void)
{
    fetch_req_t *req;
    fetch_req_t **reqp;

    lock_fetch_list("fetch_crls");
    req  =  fetch_reqs;
    reqp = &fetch_reqs;

    while (req != NULL)
    {
	bool valid_crl = FALSE;
	chunk_t blob = empty_chunk;
	generalName_t *gn = req->distributionPoints;

	while (gn != NULL)
	{
	    err_t ugh = fetch_asn1_blob(gn->name, &blob);

	    if (ugh != NULL)
	    {
		plog("fetch failed:  %s", ugh);
	    }
	    else
	    {
		chunk_t crl_uri;
		clonetochunk(crl_uri, gn->name.ptr, gn->name.len, "crl uri");
		if (insert_crl(blob, crl_uri))
		{
		    DBG(DBG_CONTROL,
			DBG_log("we have a valid crl")
		    )
		    valid_crl = TRUE;
		    break;
		}		    
	    }
	    gn = gn->next;
	}
	
	if (valid_crl)
	{
	    /* delete fetch request */
	    fetch_req_t *req_free = req;

	    req   = req->next;
	    *reqp = req;
	    free_fetch_request(req_free);	
	}
	else
	{
	    /* try again next time */
	    req->trials++;
	    reqp = &req->next;
	    req  =  req->next;
	}
    }
    unlock_fetch_list("fetch_crls");
}

static void*
fetch_thread(void *arg UNUSED)
{
    struct timespec wait_interval;

    DBG(DBG_CONTROL,
	DBG_log("fetch thread started")
    )

    pthread_mutex_lock(&fetch_wake_mutex);

    while(1)
    {
	int status;

	wait_interval.tv_nsec = 0;
	wait_interval.tv_sec = time(NULL) + crl_check_interval;

	DBG(DBG_CONTROL,
	    DBG_log("next regular crl check in %ld seconds", crl_check_interval)
	)
	status = pthread_cond_timedwait(&fetch_wake_cond, &fetch_wake_mutex
					, &wait_interval);

	if (status == ETIMEDOUT)
	{
	    DBG(DBG_CONTROL,
		DBG_log(" ");
		DBG_log("*time to check crls")
	    )
	    check_crls();
	}
	else
	{
	    DBG(DBG_CONTROL,
		DBG_log("fetch thread was woken up")
	    )
	}
	fetch_crls();
    }
}

void
init_crl_fetch(void)
{
    int status;

    if (crl_check_interval > 0)
    {
	status = pthread_create( &thread, NULL, fetch_thread, NULL);

	if (status != 0)
	{
	    plog("crl fetch thread could not be started, status = %d", status);
	}
    }
}

/*
 * add additional distribution points
 */
void
add_distribution_points(const generalName_t *newPoints ,generalName_t **distributionPoints)
{
    while (newPoints != NULL)
    {
	bool add = TRUE;
	generalName_t *gn = *distributionPoints;

	while (gn != NULL)
	{
	    if (gn->kind == newPoints->kind &&
		gn->name.len == newPoints->name.len &&
		memcmp(gn->name.ptr, newPoints->name.ptr, gn->name.len) == 0)
	    {
		/* distribution point already present, skip to next entry */
		add = FALSE;
		break;
	    }
	    gn = gn->next;
	}

	if (add)
	{
	    /* clone additional distribution point */
	    gn = clone_thing(*newPoints, "generalName");
	    clonetochunk(gn->name, newPoints->name.ptr, newPoints->name.len
		, "crl uri");

	    /* insert additional CRL distribution point */
	    gn->next = *distributionPoints;
	    *distributionPoints = gn;
	}
	newPoints = newPoints->next;
    }
}

/*
 * add a crl fetch request to the chained list
 */
void
add_fetch_request(chunk_t issuer, const generalName_t *gn)
{
    fetch_req_t *req;

    lock_fetch_list("add_fetch_request");
    req = fetch_reqs;

    while (req != NULL)
    {
	if (same_dn(issuer, req->issuer))
	{
	    /* there is already a fetch request */
	    DBG(DBG_CONTROL,
		DBG_log("fetch request already exists")
	    )

	    /* there might be new distribution points */
	    add_distribution_points(gn, &req->distributionPoints);

	    unlock_fetch_list("add_fetch_request");
	    return;
	}
	req = req->next;
    }
    /* create a new fetch request */
    req = alloc_thing(fetch_req_t, "fetch request");
    *req = empty_fetch_req;

    /* note current time */
    req->installed = time(NULL);

    /* clone issuer */
    clonetochunk(req->issuer, issuer.ptr, issuer.len, "issuer dn");

    /* copy distribution points */
    add_distribution_points(gn, &req->distributionPoints);

    /* insert new fetch request at the head of the queue */
    req->next = fetch_reqs;
    fetch_reqs = req;

    DBG(DBG_CONTROL,
	DBG_log("fetch request added")
    )
    unlock_fetch_list("add_fetch_request");
}

/*
 * list all distribution points
 */
void
list_distribution_points(const generalName_t *gn)
{
    bool first_gn = TRUE;

    while (gn != NULL)
    {
	whack_log(RC_COMMENT, "       %s '%.*s'", (first_gn)? "distPts:"
	    : "        ", (int)gn->name.len, gn->name.ptr);
	first_gn = FALSE;
	gn = gn->next;
    }
}

/*
 *  list all fetch requests in the chained list
 */
void
list_fetch_requests(bool utc)
{
    fetch_req_t *req;

    lock_fetch_list("list_fetch_requests");
    req = fetch_reqs;

    if (req != NULL)
    {
	whack_log(RC_COMMENT, " ");
	whack_log(RC_COMMENT, "List of fetch requests:");
	whack_log(RC_COMMENT, " ");
    }

    while (req != NULL)
    {
	u_char buf[BUF_LEN];

	whack_log(RC_COMMENT, "%s, trials: %d"
	    , timetoa(&req->installed, utc), req->trials);
	dntoa(buf, BUF_LEN, req->issuer);
	whack_log(RC_COMMENT, "       issuer:  '%s'", buf);
	list_distribution_points(req->distributionPoints);
	req = req->next;
    }
    unlock_fetch_list("list_fetch_requests");
}
