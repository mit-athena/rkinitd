/* Copyright 1989,1999 by the Massachusetts Institute of Technology.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 */

/* This file contains all of the kerberos part of rkinitd. */

static const char rcsid[] = "$Id: krb.c,v 1.2 1999-12-09 22:24:00 danw Exp $";

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <syslog.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <netdb.h>
#include <string.h>
#include <pwd.h>
#include <krb.h>
#include <des.h>
#include <stdlib.h>
#include <unistd.h>

#include <rkinit.h>
#include <rkinit_err.h>

#include "rkinitd.h"

#define FAILURE (!RKINIT_SUCCESS)

static char errbuf[BUFSIZ];

typedef struct {
    jmp_buf env;
} rkinitd_intkt_info;

static uid_t user_id;

static void this_phost(char *host, int hostlen)
{
    char this_host[MAXHOSTNAMELEN + 1];

    BCLEAR(this_host);

    if (gethostname(this_host, sizeof(this_host)) < 0) {
	sprintf(errbuf, "gethostname: %s", strerror(errno));
	rkinit_errmsg(errbuf);
	error();
	exit(1);
    }

    strncpy(host, krb_get_phost(this_host), hostlen - 1);
}

static int decrypt_tkt(char *user, char *instance, char *realm, char *arg,
		       int (*key_proc)(char *, char *, char *,
				       char *, C_Block),
		       KTEXT *cipp)
{
    MSG_DAT msg_data;		/* Message data containing decrypted data */
    KTEXT_ST auth;		/* Authenticator */
    AUTH_DAT auth_dat;		/* Authentication data */
    KTEXT cip = *cipp;
    MSG_DAT scip;
    int status = 0;
    des_cblock key;
    des_key_schedule sched;
    char phost[MAXHOSTNAMELEN + 1];
    struct sockaddr_in caddr;	/* client internet address */
    struct sockaddr_in saddr;	/* server internet address */

    rkinitd_intkt_info *rii = (rkinitd_intkt_info *)arg;

    u_char enc_data[MAX_KTXT_LEN];

    SBCLEAR(auth);
    SBCLEAR(auth_dat);
    SBCLEAR(scip);
    BCLEAR(enc_data);

    scip.app_data = enc_data;

    /*
     * Exchange with the client our response from the KDC (ticket encrypted
     * in user's private key) for the same ticket encrypted in our
     * (not yet known) session key.
     */

    rpc_exchange_tkt(cip, &scip);

    /*
     * Get the authenticator
     */

    SBCLEAR(auth);

    rpc_getauth(&auth, &caddr, &saddr);

    /*
     * Decode authenticator and extract session key.  The first zero
     * means we don't care what host this comes from.  This needs to
     * be done with euid of root so that /etc/srvtab can be read.
     */

    BCLEAR(phost);
    this_phost(phost, sizeof(phost));

    /*
     * This function has to use longjmp to return to the caller
     * because the kerberos library routine that calls it doesn't
     * pay attention to the return value it gives.  That means that
     * if any of these routines failed, the error returned to the client
     * would be "password incorrect".
     */

    status = krb_rd_req(&auth, KEY, phost, caddr.sin_addr.s_addr,
			    &auth_dat, "");
    if (status) {
	sprintf(errbuf, "krb_rd_req: %s", krb_err_txt[status]);
	rkinit_errmsg(errbuf);
	longjmp(rii->env, status);
    }

    memcpy(key, auth_dat.session, sizeof(key));
    if (des_key_sched(key, sched)) {
	sprintf(errbuf, "Error in des_key_sched");
	rkinit_errmsg(errbuf);
	longjmp(rii->env, RKINIT_DES);
    }

    /* Decrypt the data. */
    if ((status =
	 krb_rd_priv((u_char *)scip.app_data, scip.app_length,
		     sched, key, &caddr, &saddr, &msg_data)) == KSUCCESS) {
	cip->length = msg_data.app_length;
	memcpy(cip->dat, msg_data.app_data, msg_data.app_length);
	cip->dat[cip->length] = 0;
    }
    else {
	sprintf(errbuf, "krb_rd_priv: %s", krb_err_txt[status]);
	rkinit_errmsg(errbuf);
	longjmp(rii->env, status);
    }
    if (setuid(user_id) < 0) {
	sprintf(errbuf,	"Failure setting uid to %lu: %s\n",
		(unsigned long)user_id, strerror(errno));
	rkinit_errmsg(errbuf);
	longjmp(rii->env, RKINIT_DAEMON);
    }
    return(KSUCCESS);
}

static int validate_user(char *aname, char *inst, char *realm,
			 char *username, char *errmsg)
{
    struct passwd *pwnam;	/* For access_check and uid */
    AUTH_DAT auth_dat;
    int kstatus = KSUCCESS;

    SBCLEAR(auth_dat);

    pwnam = getpwnam(username);
    if (pwnam == NULL) {
	sprintf(errmsg, "%s does not exist on the remote host.", username);
	return(FAILURE);
    }

    strcpy(auth_dat.pname, aname);
    strcpy(auth_dat.pinst, inst);
    strcpy(auth_dat.prealm, realm);
    user_id = pwnam->pw_uid;

    kstatus = kuserok(&auth_dat, username);
    if (kstatus != KSUCCESS) {
	sprintf(errmsg, "%s has not allowed you to log in with", username);
	if (strlen(auth_dat.pinst))
	    sprintf(errmsg, "%s %s.%s", errmsg, auth_dat.pname,
		    auth_dat.pinst);
	else
	    sprintf(errmsg, "%s %s", errmsg, auth_dat.pname);
	sprintf(errmsg, "%s@%s tickets.", errmsg, auth_dat.prealm);
	return(FAILURE);
    }

    return(RKINIT_SUCCESS);
}

int get_tickets(int version)
{
    rkinit_info info;
    AUTH_DAT auth_dat;

    int status;
    char errmsg[BUFSIZ];	/* error message for client */

    rkinitd_intkt_info rii;

    SBCLEAR(info);
    SBCLEAR(auth_dat);
    BCLEAR(errmsg);
    SBCLEAR(rii);

    rpc_get_rkinit_info(&info);

    /*
     * The validate_user routine makes sure that the principal in question
     * is allowed to log in as username, and if so, does a setuid(localuid).
     * If there is an access violation or an error in setting the uid,
     * an error is returned and the string errmsg is initialized with
     * an error message that will be sent back to the client.
     */
    status = validate_user(info.aname, info.inst, info.realm,
			   info.username, errmsg);
    if (status != RKINIT_SUCCESS) {
	rpc_send_error(errmsg);
	exit(0);
    }
    else
	rpc_send_success();

    /*
     * If the name of a ticket file was specified, set it; otherwise,
     * just use the default.
     */
    if (strlen(info.tktfilename))
	krb_set_tkt_string(info.tktfilename);

    /*
     * Call internal kerberos library routine so that we can supply
     * our own ticket decryption routine.
     */

    /*
     * We need a setjmp here because krb_get_in_tkt ignores the
     * return value of decrypt_tkt.  Thus if we want any of its
     * return values to reach the client, we have to jump out of
     * the routine.
     */

    if (setjmp(rii.env) == 0) {
	status = krb_get_in_tkt(info.aname, info.inst, info.realm,
				info.sname, info.sinst, info.lifetime,
				NULL, decrypt_tkt, (char *)&rii);
	if (status) {
	    strcpy(errmsg, krb_err_txt[status]);
	    rpc_send_error(errmsg);
	}
	else
	    rpc_send_success();
    }
    else
	rpc_send_error(errbuf);

    return(RKINIT_SUCCESS);
}
