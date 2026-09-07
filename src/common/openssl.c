/*-------------------------------------------------------------------------
 *
 * openssl.c
 *		OpenSSL supporting functionality shared between frontend and backend
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/openssl.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef USE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

#include "common/openssl.h"


/*
 * Return whether FIPS mode is enabled in the underlying OpenSSL installation.
 */
bool
pg_openssl_is_fips_enabled(void)
{
#ifdef USE_OPENSSL
	/*
	 * EVP_default_properties_is_fips_enabled was added in OpenSSL 3.0, before
	 * that FIPS_mode() was used to test for FIPS being enabled.  The last
	 * upstream OpenSSL version before 3.0 which supported FIPS was 1.0.2, but
	 * there are forks of 1.1.1 which are FIPS validated so we still need to
	 * test with FIPS_mode() even though we don't support 1.0.2.
	 */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	return EVP_default_properties_is_fips_enabled(NULL) == 1;
#else
	return FIPS_mode() == 1;
#endif
#else
	return false;
#endif
}
