#include "../config.h"

#ifdef HAVE_SECURITY_PAM_MODULES_H
#define PAM_SM_SESSION
#include <security/pam_modules.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <krb5.h>
#ifdef USE_KRB4
#include KRB4_DES_H
#include KRB4_KRB_H
#ifdef KRB4_KRB_ERR_H
#include KRB4_KRB_ERR_H
#endif
#endif

#include "init.h"
#include "log.h"
#include "options.h"
#include "prompter.h"
#include "stash.h"
#include "tokens.h"
#include "userinfo.h"
#include "v5.h"
#include "v4.h"
#include "xstr.h"

#ident "$Id$"

int
pam_sm_open_session(pam_handle_t *pamh, int flags,
		    int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	PAM_KRB5_MAYBE_CONST char *user;
	char envstr[PATH_MAX + 20];
	const char *ccname;
	krb5_context ctx;
	struct _pam_krb5_options *options;
	struct _pam_krb5_user_info *userinfo;
	struct _pam_krb5_stash *stash;
	int i;

	/* Initialize Kerberos. */
	if (_pam_krb5_init_ctx(&ctx, argc, argv) != 0) {
		warn("error initializing Kerberos");
		return PAM_SERVICE_ERR;
	}

	/* Get the user's name. */
	i = pam_get_user(pamh, &user, NULL);
	if (i != PAM_SUCCESS) {
		warn("could not identify user name");
		krb5_free_context(ctx);
		return i;
	}

	/* Read our options. */
	options = _pam_krb5_options_init(pamh, argc, argv, ctx);
	if (options == NULL) {
		warn("error parsing options (shouldn't happen)");
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* Get information about the user and the user's principal name. */
	userinfo = _pam_krb5_user_info_init(ctx, user, options->realm,
					    options->user_check);
	if (userinfo == NULL) {
		if (options->debug) {
			debug("no user info for '%s'", user);
		}
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}
	if ((options->minimum_uid != -1) &&
	    (userinfo->uid < options->minimum_uid)) {
		if (options->debug) {
			debug("ignoring '%s' -- uid below minimum = %lu", user,
			      (unsigned long) options->minimum_uid);
		}
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_IGNORE;
	}

	/* Get the stash for this user. */
	stash = _pam_krb5_stash_get(pamh, userinfo);
	if (stash == NULL) {
		warn("no stash for '%s' (shouldn't happen)", user);
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* Nuke any old credential files which we have lying around. */
	v5_destroy(ctx, stash, options);
#ifdef USE_KRB4
	if (stash->v4file != NULL) {
		v4_destroy(ctx, stash, options);
	}
#endif

	/* Create credential files. */
	if (options->debug) {
		debug("creating v5 ccache for '%s'", user);
	}
	i = v5_save(ctx, stash,  userinfo, options, &ccname);
	if (i == PAM_SUCCESS) {
		if (options->debug) {
			debug("created v5 ccache '%s' for '%s'", ccname, user);
		}
		sprintf(envstr, "KRB5CCNAME=FILE:%s", ccname);
		pam_putenv(pamh, xstrdup(envstr));
	}

#ifdef USE_KRB4
	if ((i == PAM_SUCCESS) && (stash->v4present)) {
		if (options->debug) {
			debug("creating v4 ticket file for '%s'", user);
		}
		i = v4_save(ctx, stash,  userinfo, options, &ccname);
		if (i == PAM_SUCCESS) {
			if (options->debug) {
				debug("created v4 ticket file '%s' for '%s'",
				      ccname, user);
			}
			sprintf(envstr, "KRBTKFILE=%s", ccname);
			pam_putenv(pamh, xstrdup(envstr));
		}
	}
#endif

	tokens_obtain(options);

	/* Clean up. */
	if (options->debug) {
		debug("pam_open_session returning %d (%s)", i,
		      pam_strerror(pamh, i));
	}
	_pam_krb5_options_free(pamh, ctx, options);
	_pam_krb5_user_info_free(ctx, userinfo);

	/* If we didn't create ccache files because we couldn't, just
	 * pretend everything's fine. */
	if ((i != PAM_SUCCESS) &&
	    (v5_creds_check_initialized(ctx, &stash->v5creds) != 0)) {
		i = PAM_SUCCESS;
	}

	krb5_free_context(ctx);
	return i;
}

int
pam_sm_close_session(pam_handle_t *pamh, int flags,
		     int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	PAM_KRB5_MAYBE_CONST char *user;
	krb5_context ctx;
	struct _pam_krb5_options *options;
	struct _pam_krb5_user_info *userinfo;
	struct _pam_krb5_stash *stash;
	int i;

	/* Initialize Kerberos. */
	if (_pam_krb5_init_ctx(&ctx, argc, argv) != 0) {
		warn("error initializing Kerberos");
		return PAM_SERVICE_ERR;
	}

	/* Get the user's name. */
	i = pam_get_user(pamh, &user, NULL);
	if (i != PAM_SUCCESS) {
		warn("could not determine user name");
		krb5_free_context(ctx);
		return i;
	}

	/* Read our options. */
	options = _pam_krb5_options_init(pamh, argc, argv, ctx);
	if (options == NULL) {
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* Get information about the user and the user's principal name. */
	userinfo = _pam_krb5_user_info_init(ctx, user, options->realm,
					    options->user_check);
	if (userinfo == NULL) {
		warn("no user info for %s (shouldn't happen)", user);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* Check the minimum UID argument. */
	if ((options->minimum_uid != -1) &&
	    (userinfo->uid < options->minimum_uid)) {
		if (options->debug) {
			debug("ignoring '%s' -- uid below minimum", user);
		}
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_IGNORE;
	}

	/* Get the stash for this user. */
	stash = _pam_krb5_stash_get(pamh, userinfo);
	if (stash == NULL) {
		warn("no stash for user %s (shouldn't happen)", user);
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	tokens_release(options);

	v5_destroy(ctx, stash, options);

#ifdef USE_KRB4
	if (stash->v4file != NULL) {
		v4_destroy(ctx, stash, options);
	}
#endif
	if (options->debug) {
		debug("pam_close_session returning %d (%s)", 0,
		      pam_strerror(pamh, 0));
	}
	_pam_krb5_user_info_free(ctx, userinfo);
	_pam_krb5_options_free(pamh, ctx, options);
	krb5_free_context(ctx);
	return PAM_SUCCESS;
}