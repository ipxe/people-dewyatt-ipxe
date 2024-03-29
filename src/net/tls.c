/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

/**
 * @file
 *
 * Transport Layer Security Protocol
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/hmac.h>
#include <ipxe/md5.h>
#include <ipxe/sha1.h>
#include <ipxe/sha256.h>
#include <ipxe/aes.h>
#include <ipxe/rsa.h>
#include <ipxe/iobuf.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/x509.h>
#include <ipxe/clientcert.h>
#include <ipxe/rbg.h>
#include <ipxe/tls.h>

/* Disambiguate the various error causes */
#define EACCES_INCOMPLETE \
	__einfo_error ( EINFO_EACCES_INCOMPLETE )
#define EINFO_EACCES_INCOMPLETE \
	__einfo_uniqify ( EINFO_EACCES, 0x01, "Incomplete certificate chain" )
#define EACCES_WRONG_NAME \
	__einfo_error ( EINFO_EACCES_WRONG_NAME )
#define EINFO_EACCES_WRONG_NAME \
	__einfo_uniqify ( EINFO_EACCES, 0x02, "Incorrect server name" )

static int tls_send_plaintext ( struct tls_session *tls, unsigned int type,
				const void *data, size_t len );
static void tls_clear_cipher ( struct tls_session *tls,
			       struct tls_cipherspec *cipherspec );

/******************************************************************************
 *
 * Utility functions
 *
 ******************************************************************************
 */

/**
 * Extract 24-bit field value
 *
 * @v field24		24-bit field
 * @ret value		Field value
 *
 * TLS uses 24-bit integers in several places, which are awkward to
 * parse in C.
 */
static inline __attribute__ (( always_inline )) unsigned long
tls_uint24 ( const uint8_t field24[3] ) {
	const uint32_t *field32 __attribute__ (( may_alias )) =
		( ( const void * ) field24 );
	return ( be32_to_cpu ( *field32 ) >> 8 );
}

/**
 * Set 24-bit field value
 *
 * @v field24		24-bit field
 * @v value		Field value
 *
 * The field must be pre-zeroed.
 */
static void tls_set_uint24 ( uint8_t field24[3], unsigned long value ) {
	uint32_t *field32 __attribute__ (( may_alias )) =
		( ( void * ) field24 );
	*field32 |= cpu_to_be32 ( value << 8 );
}

/******************************************************************************
 *
 * Hybrid MD5+SHA1 hash as used by TLSv1.1 and earlier
 *
 ******************************************************************************
 */

/**
 * Initialise MD5+SHA1 algorithm
 *
 * @v ctx		MD5+SHA1 context
 */
static void md5_sha1_init ( void *ctx ) {
	struct md5_sha1_context *context = ctx;

	digest_init ( &md5_algorithm, context->md5 );
	digest_init ( &sha1_algorithm, context->sha1 );
}

/**
 * Accumulate data with MD5+SHA1 algorithm
 *
 * @v ctx		MD5+SHA1 context
 * @v data		Data
 * @v len		Length of data
 */
static void md5_sha1_update ( void *ctx, const void *data, size_t len ) {
	struct md5_sha1_context *context = ctx;

	digest_update ( &md5_algorithm, context->md5, data, len );
	digest_update ( &sha1_algorithm, context->sha1, data, len );
}

/**
 * Generate MD5+SHA1 digest
 *
 * @v ctx		MD5+SHA1 context
 * @v out		Output buffer
 */
static void md5_sha1_final ( void *ctx, void *out ) {
	struct md5_sha1_context *context = ctx;
	struct md5_sha1_digest *digest = out;

	digest_final ( &md5_algorithm, context->md5, digest->md5 );
	digest_final ( &sha1_algorithm, context->sha1, digest->sha1 );
}

/** Hybrid MD5+SHA1 digest algorithm */
static struct digest_algorithm md5_sha1_algorithm = {
	.name		= "md5+sha1",
	.ctxsize	= sizeof ( struct md5_sha1_context ),
	.blocksize	= 0, /* Not applicable */
	.digestsize	= sizeof ( struct md5_sha1_digest ),
	.init		= md5_sha1_init,
	.update		= md5_sha1_update,
	.final		= md5_sha1_final,
};

/** RSA digestInfo prefix for MD5+SHA1 algorithm */
struct rsa_digestinfo_prefix rsa_md5_sha1_prefix __rsa_digestinfo_prefix = {
	.digest = &md5_sha1_algorithm,
	.data = NULL, /* MD5+SHA1 signatures have no digestInfo */
	.len = 0,
};

/******************************************************************************
 *
 * Cleanup functions
 *
 ******************************************************************************
 */

/**
 * Free TLS session
 *
 * @v refcnt		Reference counter
 */
static void free_tls ( struct refcnt *refcnt ) {
	struct tls_session *tls =
		container_of ( refcnt, struct tls_session, refcnt );

	/* Free dynamically-allocated resources */
	tls_clear_cipher ( tls, &tls->tx_cipherspec );
	tls_clear_cipher ( tls, &tls->tx_cipherspec_pending );
	tls_clear_cipher ( tls, &tls->rx_cipherspec );
	tls_clear_cipher ( tls, &tls->rx_cipherspec_pending );
	free ( tls->rx_data );

	/* Free TLS structure itself */
	free ( tls );	
}

/**
 * Finish with TLS session
 *
 * @v tls		TLS session
 * @v rc		Status code
 */
static void tls_close ( struct tls_session *tls, int rc ) {

	/* Remove process */
	process_del ( &tls->process );
	
	/* Close ciphertext and plaintext streams */
	intf_shutdown ( &tls->cipherstream, rc );
	intf_shutdown ( &tls->plainstream, rc );
}

/******************************************************************************
 *
 * Random number generation
 *
 ******************************************************************************
 */

/**
 * Generate random data
 *
 * @v tls		TLS session
 * @v data		Buffer to fill
 * @v len		Length of buffer
 * @ret rc		Return status code
 */
static int tls_generate_random ( struct tls_session *tls,
				 void *data, size_t len ) {
	int rc;

	/* Generate random bits with no additional input and without
	 * prediction resistance
	 */
	if ( ( rc = rbg_generate ( NULL, 0, 0, data, len ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not generate random data: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Update HMAC with a list of ( data, len ) pairs
 *
 * @v digest		Hash function to use
 * @v digest_ctx	Digest context
 * @v args		( data, len ) pairs of data, terminated by NULL
 */
static void tls_hmac_update_va ( struct digest_algorithm *digest,
				 void *digest_ctx, va_list args ) {
	void *data;
	size_t len;

	while ( ( data = va_arg ( args, void * ) ) ) {
		len = va_arg ( args, size_t );
		hmac_update ( digest, digest_ctx, data, len );
	}
}

/**
 * Generate secure pseudo-random data using a single hash function
 *
 * @v tls		TLS session
 * @v digest		Hash function to use
 * @v secret		Secret
 * @v secret_len	Length of secret
 * @v out		Output buffer
 * @v out_len		Length of output buffer
 * @v seeds		( data, len ) pairs of seed data, terminated by NULL
 */
static void tls_p_hash_va ( struct tls_session *tls,
			    struct digest_algorithm *digest,
			    void *secret, size_t secret_len,
			    void *out, size_t out_len,
			    va_list seeds ) {
	uint8_t secret_copy[secret_len];
	uint8_t digest_ctx[digest->ctxsize];
	uint8_t digest_ctx_partial[digest->ctxsize];
	uint8_t a[digest->digestsize];
	uint8_t out_tmp[digest->digestsize];
	size_t frag_len = digest->digestsize;
	va_list tmp;

	/* Copy the secret, in case HMAC modifies it */
	memcpy ( secret_copy, secret, secret_len );
	secret = secret_copy;
	DBGC2 ( tls, "TLS %p %s secret:\n", tls, digest->name );
	DBGC2_HD ( tls, secret, secret_len );

	/* Calculate A(1) */
	hmac_init ( digest, digest_ctx, secret, &secret_len );
	va_copy ( tmp, seeds );
	tls_hmac_update_va ( digest, digest_ctx, tmp );
	va_end ( tmp );
	hmac_final ( digest, digest_ctx, secret, &secret_len, a );
	DBGC2 ( tls, "TLS %p %s A(1):\n", tls, digest->name );
	DBGC2_HD ( tls, &a, sizeof ( a ) );

	/* Generate as much data as required */
	while ( out_len ) {
		/* Calculate output portion */
		hmac_init ( digest, digest_ctx, secret, &secret_len );
		hmac_update ( digest, digest_ctx, a, sizeof ( a ) );
		memcpy ( digest_ctx_partial, digest_ctx, digest->ctxsize );
		va_copy ( tmp, seeds );
		tls_hmac_update_va ( digest, digest_ctx, tmp );
		va_end ( tmp );
		hmac_final ( digest, digest_ctx,
			     secret, &secret_len, out_tmp );

		/* Copy output */
		if ( frag_len > out_len )
			frag_len = out_len;
		memcpy ( out, out_tmp, frag_len );
		DBGC2 ( tls, "TLS %p %s output:\n", tls, digest->name );
		DBGC2_HD ( tls, out, frag_len );

		/* Calculate A(i) */
		hmac_final ( digest, digest_ctx_partial,
			     secret, &secret_len, a );
		DBGC2 ( tls, "TLS %p %s A(n):\n", tls, digest->name );
		DBGC2_HD ( tls, &a, sizeof ( a ) );

		out += frag_len;
		out_len -= frag_len;
	}
}

/**
 * Generate secure pseudo-random data
 *
 * @v tls		TLS session
 * @v secret		Secret
 * @v secret_len	Length of secret
 * @v out		Output buffer
 * @v out_len		Length of output buffer
 * @v ...		( data, len ) pairs of seed data, terminated by NULL
 */
static void tls_prf ( struct tls_session *tls, void *secret, size_t secret_len,
		      void *out, size_t out_len, ... ) {
	va_list seeds;
	va_list tmp;
	size_t subsecret_len;
	void *md5_secret;
	void *sha1_secret;
	uint8_t buf[out_len];
	unsigned int i;

	va_start ( seeds, out_len );

	if ( tls->version >= TLS_VERSION_TLS_1_2 ) {
		/* Use P_SHA256 for TLSv1.2 and later */
		tls_p_hash_va ( tls, &sha256_algorithm, secret, secret_len,
				out, out_len, seeds );
	} else {
		/* Use combination of P_MD5 and P_SHA-1 for TLSv1.1
		 * and earlier
		 */

		/* Split secret into two, with an overlap of up to one byte */
		subsecret_len = ( ( secret_len + 1 ) / 2 );
		md5_secret = secret;
		sha1_secret = ( secret + secret_len - subsecret_len );

		/* Calculate MD5 portion */
		va_copy ( tmp, seeds );
		tls_p_hash_va ( tls, &md5_algorithm, md5_secret,
				subsecret_len, out, out_len, seeds );
		va_end ( tmp );

		/* Calculate SHA1 portion */
		va_copy ( tmp, seeds );
		tls_p_hash_va ( tls, &sha1_algorithm, sha1_secret,
				subsecret_len, buf, out_len, seeds );
		va_end ( tmp );

		/* XOR the two portions together into the final output buffer */
		for ( i = 0 ; i < out_len ; i++ )
			*( ( uint8_t * ) out + i ) ^= buf[i];
	}

	va_end ( seeds );
}

/**
 * Generate secure pseudo-random data
 *
 * @v secret		Secret
 * @v secret_len	Length of secret
 * @v out		Output buffer
 * @v out_len		Length of output buffer
 * @v label		String literal label
 * @v ...		( data, len ) pairs of seed data
 */
#define tls_prf_label( tls, secret, secret_len, out, out_len, label, ... ) \
	tls_prf ( (tls), (secret), (secret_len), (out), (out_len),	   \
		  label, ( sizeof ( label ) - 1 ), __VA_ARGS__, NULL )

/******************************************************************************
 *
 * Secret management
 *
 ******************************************************************************
 */

/**
 * Generate master secret
 *
 * @v tls		TLS session
 *
 * The pre-master secret and the client and server random values must
 * already be known.
 */
static void tls_generate_master_secret ( struct tls_session *tls ) {
	DBGC ( tls, "TLS %p pre-master-secret:\n", tls );
	DBGC_HD ( tls, &tls->pre_master_secret,
		  sizeof ( tls->pre_master_secret ) );
	DBGC ( tls, "TLS %p client random bytes:\n", tls );
	DBGC_HD ( tls, &tls->client_random, sizeof ( tls->client_random ) );
	DBGC ( tls, "TLS %p server random bytes:\n", tls );
	DBGC_HD ( tls, &tls->server_random, sizeof ( tls->server_random ) );

	tls_prf_label ( tls, &tls->pre_master_secret,
			sizeof ( tls->pre_master_secret ),
			&tls->master_secret, sizeof ( tls->master_secret ),
			"master secret",
			&tls->client_random, sizeof ( tls->client_random ),
			&tls->server_random, sizeof ( tls->server_random ) );

	DBGC ( tls, "TLS %p generated master secret:\n", tls );
	DBGC_HD ( tls, &tls->master_secret, sizeof ( tls->master_secret ) );
}

/**
 * Generate key material
 *
 * @v tls		TLS session
 *
 * The master secret must already be known.
 */
static int tls_generate_keys ( struct tls_session *tls ) {
	struct tls_cipherspec *tx_cipherspec = &tls->tx_cipherspec_pending;
	struct tls_cipherspec *rx_cipherspec = &tls->rx_cipherspec_pending;
	size_t hash_size = tx_cipherspec->suite->digest->digestsize;
	size_t key_size = tx_cipherspec->suite->key_len;
	size_t iv_size = tx_cipherspec->suite->cipher->blocksize;
	size_t total = ( 2 * ( hash_size + key_size + iv_size ) );
	uint8_t key_block[total];
	uint8_t *key;
	int rc;

	/* Generate key block */
	tls_prf_label ( tls, &tls->master_secret, sizeof ( tls->master_secret ),
			key_block, sizeof ( key_block ), "key expansion",
			&tls->server_random, sizeof ( tls->server_random ),
			&tls->client_random, sizeof ( tls->client_random ) );

	/* Split key block into portions */
	key = key_block;

	/* TX MAC secret */
	memcpy ( tx_cipherspec->mac_secret, key, hash_size );
	DBGC ( tls, "TLS %p TX MAC secret:\n", tls );
	DBGC_HD ( tls, key, hash_size );
	key += hash_size;

	/* RX MAC secret */
	memcpy ( rx_cipherspec->mac_secret, key, hash_size );
	DBGC ( tls, "TLS %p RX MAC secret:\n", tls );
	DBGC_HD ( tls, key, hash_size );
	key += hash_size;

	/* TX key */
	if ( ( rc = cipher_setkey ( tx_cipherspec->suite->cipher,
				    tx_cipherspec->cipher_ctx,
				    key, key_size ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not set TX key: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}
	DBGC ( tls, "TLS %p TX key:\n", tls );
	DBGC_HD ( tls, key, key_size );
	key += key_size;

	/* RX key */
	if ( ( rc = cipher_setkey ( rx_cipherspec->suite->cipher,
				    rx_cipherspec->cipher_ctx,
				    key, key_size ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not set TX key: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}
	DBGC ( tls, "TLS %p RX key:\n", tls );
	DBGC_HD ( tls, key, key_size );
	key += key_size;

	/* TX initialisation vector */
	cipher_setiv ( tx_cipherspec->suite->cipher,
		       tx_cipherspec->cipher_ctx, key );
	DBGC ( tls, "TLS %p TX IV:\n", tls );
	DBGC_HD ( tls, key, iv_size );
	key += iv_size;

	/* RX initialisation vector */
	cipher_setiv ( rx_cipherspec->suite->cipher,
		       rx_cipherspec->cipher_ctx, key );
	DBGC ( tls, "TLS %p RX IV:\n", tls );
	DBGC_HD ( tls, key, iv_size );
	key += iv_size;

	assert ( ( key_block + total ) == key );

	return 0;
}

/******************************************************************************
 *
 * Cipher suite management
 *
 ******************************************************************************
 */

/** Null cipher suite */
struct tls_cipher_suite tls_cipher_suite_null = {
	.pubkey = &pubkey_null,
	.cipher = &cipher_null,
	.digest = &digest_null,
};

/** Supported cipher suites, in order of preference */
struct tls_cipher_suite tls_cipher_suites[] = {
	{
		.code = htons ( TLS_RSA_WITH_AES_256_CBC_SHA256 ),
		.key_len = ( 256 / 8 ),
		.pubkey = &rsa_algorithm,
		.cipher = &aes_cbc_algorithm,
		.digest = &sha256_algorithm,
	},
	{
		.code = htons ( TLS_RSA_WITH_AES_128_CBC_SHA256 ),
		.key_len = ( 128 / 8 ),
		.pubkey = &rsa_algorithm,
		.cipher = &aes_cbc_algorithm,
		.digest = &sha256_algorithm,
	},
	{
		.code = htons ( TLS_RSA_WITH_AES_256_CBC_SHA ),
		.key_len = ( 256 / 8 ),
		.pubkey = &rsa_algorithm,
		.cipher = &aes_cbc_algorithm,
		.digest = &sha1_algorithm,
	},
	{
		.code = htons ( TLS_RSA_WITH_AES_128_CBC_SHA ),
		.key_len = ( 128 / 8 ),
		.pubkey = &rsa_algorithm,
		.cipher = &aes_cbc_algorithm,
		.digest = &sha1_algorithm,
	},
};

/** Number of supported cipher suites */
#define TLS_NUM_CIPHER_SUITES \
	( sizeof ( tls_cipher_suites ) / sizeof ( tls_cipher_suites[0] ) )

/**
 * Identify cipher suite
 *
 * @v cipher_suite	Cipher suite specification
 * @ret suite		Cipher suite, or NULL
 */
static struct tls_cipher_suite *
tls_find_cipher_suite ( unsigned int cipher_suite ) {
	struct tls_cipher_suite *suite;
	unsigned int i;

	/* Identify cipher suite */
	for ( i = 0 ; i < TLS_NUM_CIPHER_SUITES ; i++ ) {
		suite = &tls_cipher_suites[i];
		if ( suite->code == cipher_suite )
			return suite;
	}

	return NULL;
}

/**
 * Clear cipher suite
 *
 * @v cipherspec	TLS cipher specification
 */
static void tls_clear_cipher ( struct tls_session *tls __unused,
			       struct tls_cipherspec *cipherspec ) {

	if ( cipherspec->suite ) {
		pubkey_final ( cipherspec->suite->pubkey,
			       cipherspec->pubkey_ctx );
	}
	free ( cipherspec->dynamic );
	memset ( cipherspec, 0, sizeof ( *cipherspec ) );
	cipherspec->suite = &tls_cipher_suite_null;
}

/**
 * Set cipher suite
 *
 * @v tls		TLS session
 * @v cipherspec	TLS cipher specification
 * @v suite		Cipher suite
 * @ret rc		Return status code
 */
static int tls_set_cipher ( struct tls_session *tls,
			    struct tls_cipherspec *cipherspec,
			    struct tls_cipher_suite *suite ) {
	struct pubkey_algorithm *pubkey = suite->pubkey;
	struct cipher_algorithm *cipher = suite->cipher;
	struct digest_algorithm *digest = suite->digest;
	size_t total;
	void *dynamic;

	/* Clear out old cipher contents, if any */
	tls_clear_cipher ( tls, cipherspec );
	
	/* Allocate dynamic storage */
	total = ( pubkey->ctxsize + 2 * cipher->ctxsize + digest->digestsize );
	dynamic = zalloc ( total );
	if ( ! dynamic ) {
		DBGC ( tls, "TLS %p could not allocate %zd bytes for crypto "
		       "context\n", tls, total );
		return -ENOMEM;
	}

	/* Assign storage */
	cipherspec->dynamic = dynamic;
	cipherspec->pubkey_ctx = dynamic;	dynamic += pubkey->ctxsize;
	cipherspec->cipher_ctx = dynamic;	dynamic += cipher->ctxsize;
	cipherspec->cipher_next_ctx = dynamic;	dynamic += cipher->ctxsize;
	cipherspec->mac_secret = dynamic;	dynamic += digest->digestsize;
	assert ( ( cipherspec->dynamic + total ) == dynamic );

	/* Store parameters */
	cipherspec->suite = suite;

	return 0;
}

/**
 * Select next cipher suite
 *
 * @v tls		TLS session
 * @v cipher_suite	Cipher suite specification
 * @ret rc		Return status code
 */
static int tls_select_cipher ( struct tls_session *tls,
			       unsigned int cipher_suite ) {
	struct tls_cipher_suite *suite;
	int rc;

	/* Identify cipher suite */
	suite = tls_find_cipher_suite ( cipher_suite );
	if ( ! suite ) {
		DBGC ( tls, "TLS %p does not support cipher %04x\n",
		       tls, ntohs ( cipher_suite ) );
		return -ENOTSUP;
	}

	/* Set ciphers */
	if ( ( rc = tls_set_cipher ( tls, &tls->tx_cipherspec_pending,
				     suite ) ) != 0 )
		return rc;
	if ( ( rc = tls_set_cipher ( tls, &tls->rx_cipherspec_pending,
				     suite ) ) != 0 )
		return rc;

	DBGC ( tls, "TLS %p selected %s-%s-%d-%s\n", tls, suite->pubkey->name,
	       suite->cipher->name, ( suite->key_len * 8 ),
	       suite->digest->name );

	return 0;
}

/**
 * Activate next cipher suite
 *
 * @v tls		TLS session
 * @v pending		Pending cipher specification
 * @v active		Active cipher specification to replace
 * @ret rc		Return status code
 */
static int tls_change_cipher ( struct tls_session *tls,
			       struct tls_cipherspec *pending,
			       struct tls_cipherspec *active ) {

	/* Sanity check */
	if ( pending->suite == &tls_cipher_suite_null ) {
		DBGC ( tls, "TLS %p refusing to use null cipher\n", tls );
		return -ENOTSUP;
	}

	tls_clear_cipher ( tls, active );
	memswap ( active, pending, sizeof ( *active ) );
	return 0;
}

/******************************************************************************
 *
 * Signature and hash algorithms
 *
 ******************************************************************************
 */

/** Supported signature and hash algorithms
 *
 * Note that the default (TLSv1.1 and earlier) algorithm using
 * MD5+SHA1 is never explicitly specified.
 */
struct tls_signature_hash_algorithm tls_signature_hash_algorithms[] = {
	{
		.code = {
			.signature = TLS_RSA_ALGORITHM,
			.hash = TLS_SHA256_ALGORITHM,
		},
		.pubkey = &rsa_algorithm,
		.digest = &sha256_algorithm,
	},
};

/** Number of supported signature and hash algorithms */
#define TLS_NUM_SIG_HASH_ALGORITHMS			\
	( sizeof ( tls_signature_hash_algorithms ) /	\
	  sizeof ( tls_signature_hash_algorithms[0] ) )

/**
 * Find TLS signature and hash algorithm
 *
 * @v pubkey		Public-key algorithm
 * @v digest		Digest algorithm
 * @ret sig_hash	Signature and hash algorithm, or NULL
 */
static struct tls_signature_hash_algorithm *
tls_signature_hash_algorithm ( struct pubkey_algorithm *pubkey,
			       struct digest_algorithm *digest ) {
	struct tls_signature_hash_algorithm *sig_hash;
	unsigned int i;

	/* Identify signature and hash algorithm */
	for ( i = 0 ; i < TLS_NUM_SIG_HASH_ALGORITHMS ; i++ ) {
		sig_hash = &tls_signature_hash_algorithms[i];
		if ( ( sig_hash->pubkey == pubkey ) &&
		     ( sig_hash->digest == digest ) ) {
			return sig_hash;
		}
	}

	return NULL;
}

/******************************************************************************
 *
 * Handshake verification
 *
 ******************************************************************************
 */

/**
 * Add handshake record to verification hash
 *
 * @v tls		TLS session
 * @v data		Handshake record
 * @v len		Length of handshake record
 */
static void tls_add_handshake ( struct tls_session *tls,
				const void *data, size_t len ) {

	digest_update ( &md5_sha1_algorithm, tls->handshake_md5_sha1_ctx,
			data, len );
	digest_update ( &sha256_algorithm, tls->handshake_sha256_ctx,
			data, len );
}

/**
 * Calculate handshake verification hash
 *
 * @v tls		TLS session
 * @v out		Output buffer
 *
 * Calculates the MD5+SHA1 or SHA256 digest over all handshake
 * messages seen so far.
 */
static void tls_verify_handshake ( struct tls_session *tls, void *out ) {
	struct digest_algorithm *digest = tls->handshake_digest;
	uint8_t ctx[ digest->ctxsize ];

	memcpy ( ctx, tls->handshake_ctx, sizeof ( ctx ) );
	digest_final ( digest, ctx, out );
}

/******************************************************************************
 *
 * Record handling
 *
 ******************************************************************************
 */

/**
 * Resume TX state machine
 *
 * @v tls		TLS session
 */
static void tls_tx_resume ( struct tls_session *tls ) {
	process_add ( &tls->process );
}

/**
 * Transmit Handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_send_handshake ( struct tls_session *tls,
				void *data, size_t len ) {

	/* Add to handshake digest */
	tls_add_handshake ( tls, data, len );

	/* Send record */
	return tls_send_plaintext ( tls, TLS_TYPE_HANDSHAKE, data, len );
}

/**
 * Transmit Client Hello record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_client_hello ( struct tls_session *tls ) {
	struct {
		uint32_t type_length;
		uint16_t version;
		uint8_t random[32];
		uint8_t session_id_len;
		uint16_t cipher_suite_len;
		uint16_t cipher_suites[TLS_NUM_CIPHER_SUITES];
		uint8_t compression_methods_len;
		uint8_t compression_methods[1];
		uint16_t extensions_len;
		struct {
			uint16_t server_name_type;
			uint16_t server_name_len;
			struct {
				uint16_t len;
				struct {
					uint8_t type;
					uint16_t len;
					uint8_t name[ strlen ( tls->name ) ];
				} __attribute__ (( packed )) list[1];
			} __attribute__ (( packed )) server_name;
		} __attribute__ (( packed )) extensions;
	} __attribute__ (( packed )) hello;
	unsigned int i;

	memset ( &hello, 0, sizeof ( hello ) );
	hello.type_length = ( cpu_to_le32 ( TLS_CLIENT_HELLO ) |
			      htonl ( sizeof ( hello ) -
				      sizeof ( hello.type_length ) ) );
	hello.version = htons ( tls->version );
	memcpy ( &hello.random, &tls->client_random, sizeof ( hello.random ) );
	hello.cipher_suite_len = htons ( sizeof ( hello.cipher_suites ) );
	for ( i = 0 ; i < TLS_NUM_CIPHER_SUITES ; i++ )
		hello.cipher_suites[i] = tls_cipher_suites[i].code;
	hello.compression_methods_len = sizeof ( hello.compression_methods );
	hello.extensions_len = htons ( sizeof ( hello.extensions ) );
	hello.extensions.server_name_type = htons ( TLS_SERVER_NAME );
	hello.extensions.server_name_len
		= htons ( sizeof ( hello.extensions.server_name ) );
	hello.extensions.server_name.len
		= htons ( sizeof ( hello.extensions.server_name.list ) );
	hello.extensions.server_name.list[0].type = TLS_SERVER_NAME_HOST_NAME;
	hello.extensions.server_name.list[0].len
		= htons ( sizeof ( hello.extensions.server_name.list[0].name ));
	memcpy ( hello.extensions.server_name.list[0].name, tls->name,
		 sizeof ( hello.extensions.server_name.list[0].name ) );

	return tls_send_handshake ( tls, &hello, sizeof ( hello ) );
}

/**
 * Transmit Certificate record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_certificate ( struct tls_session *tls ) {
	int num_certificates = ( have_client_certificate() ? 1 : 0 );
	struct {
		uint32_t type_length;
		uint8_t length[3];
		struct {
			uint8_t length[3];
			uint8_t data[ client_certificate.len ];
		} __attribute__ (( packed )) certificates[num_certificates];
	} __attribute__ (( packed )) *certificate;
	struct x509_certificate cert;
	int rc;

	/* If we have a certificate to send, determine the applicable
	 * public-key algorithm and schedule transmission of
	 * CertificateVerify.
	 */
	if ( num_certificates ) {

		/* Parse certificate to determine public-key algorithm */
		if ( ( rc = x509_parse ( &cert, client_certificate.data,
					 client_certificate.len ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not parse client "
			       "certificate: %s\n", tls, strerror ( rc ) );
			return rc;
		}
		tls->verify_pubkey = cert.signature_algorithm->pubkey;

		/* Schedule CertificateVerify transmission */
		tls->tx_pending |= TLS_TX_CERTIFICATE_VERIFY;
		tls_tx_resume ( tls );
	}

	/* Allocate storage for Certificate record (which may be too
	 * large for the stack).
	 */
	certificate = zalloc ( sizeof ( *certificate ) );
	if ( ! certificate )
		return -ENOMEM;

	/* Populate record */
	certificate->type_length =
		( cpu_to_le32 ( TLS_CERTIFICATE ) |
		  htonl ( sizeof ( *certificate ) -
			  sizeof ( certificate->type_length ) ) );
	tls_set_uint24 ( certificate->length,
			 sizeof ( certificate->certificates ) );
	if ( num_certificates ) {
		tls_set_uint24 ( certificate->certificates[0].length,
				 sizeof ( certificate->certificates[0].data ) );
		memcpy ( certificate->certificates[0].data,
			 client_certificate.data,
			 sizeof ( certificate->certificates[0].data ) );
	}

	/* Transmit record */
	rc = tls_send_handshake ( tls, certificate, sizeof ( *certificate ) );

	/* Free record */
	free ( certificate );

	return rc;
}

/**
 * Transmit Client Key Exchange record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_client_key_exchange ( struct tls_session *tls ) {
	struct tls_cipherspec *cipherspec = &tls->tx_cipherspec_pending;
	struct pubkey_algorithm *pubkey = cipherspec->suite->pubkey;
	size_t max_len = pubkey_max_len ( pubkey, cipherspec->pubkey_ctx );
	struct {
		uint32_t type_length;
		uint16_t encrypted_pre_master_secret_len;
		uint8_t encrypted_pre_master_secret[max_len];
	} __attribute__ (( packed )) key_xchg;
	size_t unused;
	int len;
	int rc;

	/* Encrypt pre-master secret using server's public key */
	memset ( &key_xchg, 0, sizeof ( key_xchg ) );
	len = pubkey_encrypt ( pubkey, cipherspec->pubkey_ctx,
			       &tls->pre_master_secret,
			       sizeof ( tls->pre_master_secret ),
			       key_xchg.encrypted_pre_master_secret );
	if ( len < 0 ) {
		rc = len;
		DBGC ( tls, "TLS %p could not encrypt pre-master secret: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}
	unused = ( max_len - len );
	key_xchg.type_length =
		( cpu_to_le32 ( TLS_CLIENT_KEY_EXCHANGE ) |
		  htonl ( sizeof ( key_xchg ) -
			  sizeof ( key_xchg.type_length ) - unused ) );
	key_xchg.encrypted_pre_master_secret_len =
		htons ( sizeof ( key_xchg.encrypted_pre_master_secret ) -
			unused );

	return tls_send_handshake ( tls, &key_xchg,
				    ( sizeof ( key_xchg ) - unused ) );
}

/**
 * Transmit Certificate Verify record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_certificate_verify ( struct tls_session *tls ) {
	struct digest_algorithm *digest = tls->handshake_digest;
	struct pubkey_algorithm *pubkey = tls->verify_pubkey;
	uint8_t digest_out[ digest->digestsize ];
	uint8_t ctx[ pubkey->ctxsize ];
	struct tls_signature_hash_algorithm *sig_hash = NULL;
	int rc;

	/* Generate digest to be signed */
	tls_verify_handshake ( tls, digest_out );

	/* Initialise public-key algorithm */
	if ( ( rc = pubkey_init ( pubkey, ctx, client_private_key.data,
				  client_private_key.len ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not initialise %s client private "
		       "key: %s\n", tls, pubkey->name, strerror ( rc ) );
		goto err_pubkey_init;
	}

	/* TLSv1.2 and later use explicit algorithm identifiers */
	if ( tls->version >= TLS_VERSION_TLS_1_2 ) {
		sig_hash = tls_signature_hash_algorithm ( pubkey, digest );
		if ( ! sig_hash ) {
			DBGC ( tls, "TLS %p could not identify (%s,%s) "
			       "signature and hash algorithm\n", tls,
			       pubkey->name, digest->name );
			rc = -ENOTSUP;
			goto err_sig_hash;
		}
	}

	/* Generate and transmit record */
	{
		size_t max_len = pubkey_max_len ( pubkey, ctx );
		int use_sig_hash = ( ( sig_hash == NULL ) ? 0 : 1 );
		struct {
			uint32_t type_length;
			struct tls_signature_hash_id sig_hash[use_sig_hash];
			uint16_t signature_len;
			uint8_t signature[max_len];
		} __attribute__ (( packed )) certificate_verify;
		size_t unused;
		int len;

		/* Sign digest */
		len = pubkey_sign ( pubkey, ctx, digest, digest_out,
				    certificate_verify.signature );
		if ( len < 0 ) {
			rc = len;
			DBGC ( tls, "TLS %p could not sign %s digest using %s "
			       "client private key: %s\n", tls, digest->name,
			       pubkey->name, strerror ( rc ) );
			goto err_pubkey_sign;
		}
		unused = ( max_len - len );

		/* Construct Certificate Verify record */
		certificate_verify.type_length =
			( cpu_to_le32 ( TLS_CERTIFICATE_VERIFY ) |
			  htonl ( sizeof ( certificate_verify ) -
				  sizeof ( certificate_verify.type_length ) -
				  unused ) );
		if ( use_sig_hash ) {
			memcpy ( &certificate_verify.sig_hash[0],
				 &sig_hash->code,
				 sizeof ( certificate_verify.sig_hash[0] ) );
		}
		certificate_verify.signature_len =
			htons ( sizeof ( certificate_verify.signature ) -
				unused );

		/* Transmit record */
		rc = tls_send_handshake ( tls, &certificate_verify,
				   ( sizeof ( certificate_verify ) - unused ) );
	}

 err_pubkey_sign:
 err_sig_hash:
	pubkey_final ( pubkey, ctx );
 err_pubkey_init:
	return rc;
}

/**
 * Transmit Change Cipher record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_change_cipher ( struct tls_session *tls ) {
	static const uint8_t change_cipher[1] = { 1 };
	return tls_send_plaintext ( tls, TLS_TYPE_CHANGE_CIPHER,
				    change_cipher, sizeof ( change_cipher ) );
}

/**
 * Transmit Finished record
 *
 * @v tls		TLS session
 * @ret rc		Return status code
 */
static int tls_send_finished ( struct tls_session *tls ) {
	struct digest_algorithm *digest = tls->handshake_digest;
	struct {
		uint32_t type_length;
		uint8_t verify_data[12];
	} __attribute__ (( packed )) finished;
	uint8_t digest_out[ digest->digestsize ];

	memset ( &finished, 0, sizeof ( finished ) );
	finished.type_length = ( cpu_to_le32 ( TLS_FINISHED ) |
				 htonl ( sizeof ( finished ) -
					 sizeof ( finished.type_length ) ) );
	tls_verify_handshake ( tls, digest_out );
	tls_prf_label ( tls, &tls->master_secret, sizeof ( tls->master_secret ),
			finished.verify_data, sizeof ( finished.verify_data ),
			"client finished", digest_out, sizeof ( digest_out ) );

	return tls_send_handshake ( tls, &finished, sizeof ( finished ) );
}

/**
 * Receive new Change Cipher record
 *
 * @v tls		TLS session
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_new_change_cipher ( struct tls_session *tls,
				   const void *data, size_t len ) {
	int rc;

	if ( ( len != 1 ) || ( *( ( uint8_t * ) data ) != 1 ) ) {
		DBGC ( tls, "TLS %p received invalid Change Cipher\n", tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	if ( ( rc = tls_change_cipher ( tls, &tls->rx_cipherspec_pending,
					&tls->rx_cipherspec ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not activate RX cipher: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}
	tls->rx_seq = ~( ( uint64_t ) 0 );

	return 0;
}

/**
 * Receive new Alert record
 *
 * @v tls		TLS session
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_new_alert ( struct tls_session *tls, const void *data,
			   size_t len ) {
	const struct {
		uint8_t level;
		uint8_t description;
		char next[0];
	} __attribute__ (( packed )) *alert = data;
	const void *end = alert->next;

	/* Sanity check */
	if ( end != ( data + len ) ) {
		DBGC ( tls, "TLS %p received overlength Alert\n", tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	switch ( alert->level ) {
	case TLS_ALERT_WARNING:
		DBGC ( tls, "TLS %p received warning alert %d\n",
		       tls, alert->description );
		return 0;
	case TLS_ALERT_FATAL:
		DBGC ( tls, "TLS %p received fatal alert %d\n",
		       tls, alert->description );
		return -EPERM;
	default:
		DBGC ( tls, "TLS %p received unknown alert level %d"
		       "(alert %d)\n", tls, alert->level, alert->description );
		return -EIO;
	}
}

/**
 * Receive new Server Hello handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext handshake record
 * @v len		Length of plaintext handshake record
 * @ret rc		Return status code
 */
static int tls_new_server_hello ( struct tls_session *tls,
				  const void *data, size_t len ) {
	const struct {
		uint16_t version;
		uint8_t random[32];
		uint8_t session_id_len;
		char next[0];
	} __attribute__ (( packed )) *hello_a = data;
	const struct {
		uint8_t session_id[hello_a->session_id_len];
		uint16_t cipher_suite;
		uint8_t compression_method;
		char next[0];
	} __attribute__ (( packed )) *hello_b = ( void * ) &hello_a->next;
	const void *end = hello_b->next;
	uint16_t version;
	int rc;

	/* Sanity check */
	if ( end > ( data + len ) ) {
		DBGC ( tls, "TLS %p received underlength Server Hello\n", tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	/* Check and store protocol version */
	version = ntohs ( hello_a->version );
	if ( version < TLS_VERSION_TLS_1_0 ) {
		DBGC ( tls, "TLS %p does not support protocol version %d.%d\n",
		       tls, ( version >> 8 ), ( version & 0xff ) );
		return -ENOTSUP;
	}
	if ( version > tls->version ) {
		DBGC ( tls, "TLS %p server attempted to illegally upgrade to "
		       "protocol version %d.%d\n",
		       tls, ( version >> 8 ), ( version & 0xff ) );
		return -EPROTO;
	}
	tls->version = version;
	DBGC ( tls, "TLS %p using protocol version %d.%d\n",
	       tls, ( version >> 8 ), ( version & 0xff ) );

	/* Use MD5+SHA1 digest algorithm for handshake verification
	 * for versions earlier than TLSv1.2.
	 */
	if ( tls->version < TLS_VERSION_TLS_1_2 ) {
		tls->handshake_digest = &md5_sha1_algorithm;
		tls->handshake_ctx = tls->handshake_md5_sha1_ctx;
	}

	/* Copy out server random bytes */
	memcpy ( &tls->server_random, &hello_a->random,
		 sizeof ( tls->server_random ) );

	/* Select cipher suite */
	if ( ( rc = tls_select_cipher ( tls, hello_b->cipher_suite ) ) != 0 )
		return rc;

	/* Generate secrets */
	tls_generate_master_secret ( tls );
	if ( ( rc = tls_generate_keys ( tls ) ) != 0 )
		return rc;

	return 0;
}

/** TLS certificate chain context */
struct tls_certificate_context {
	/** TLS session */
	struct tls_session *tls;
	/** Current certificate */
	const void *current;
	/** End of certificates */
	const void *end;
};

/**
 * Parse next certificate in TLS certificate list
 *
 * @v cert		X.509 certificate to fill in
 * @v previous		Previous X.509 certificate, or NULL
 * @v ctx		Context
 * @ret rc		Return status code
 */
static int tls_parse_next ( struct x509_certificate *cert,
			    const struct x509_certificate *previous __unused,
			    void *ctx ) {
	struct tls_certificate_context *context = ctx;
	struct tls_session *tls = context->tls;
	const struct {
		uint8_t length[3];
		uint8_t certificate[0];
	} __attribute__ (( packed )) *current = context->current;
	const void *data;
	const void *next;
	size_t len;
	int rc;

	/* Return error at end of chain */
	if ( context->current >= context->end ) {
		DBGC ( tls, "TLS %p reached end of certificate chain\n", tls );
		return -EACCES_INCOMPLETE;
	}

	/* Extract current certificate and update context */
	data = current->certificate;
	len = tls_uint24 ( current->length );
	next = ( data + len );
	if ( next > context->end ) {
		DBGC ( tls, "TLS %p overlength certificate\n", tls );
		DBGC_HDA ( tls, 0, context->current,
			   ( context->end - context->current ) );
		return -EINVAL;
	}
	context->current = next;

	/* Parse current certificate */
	if ( ( rc = x509_parse ( cert, data, len ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not parse certificate: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Receive new Certificate handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext handshake record
 * @v len		Length of plaintext handshake record
 * @ret rc		Return status code
 */
static int tls_new_certificate ( struct tls_session *tls,
				 const void *data, size_t len ) {
	const struct {
		uint8_t length[3];
		uint8_t certificates[0];
	} __attribute__ (( packed )) *certificate = data;
	size_t elements_len = tls_uint24 ( certificate->length );
	const void *end = ( certificate->certificates + elements_len );
	struct tls_cipherspec *cipherspec = &tls->tx_cipherspec_pending;
	struct pubkey_algorithm *pubkey = cipherspec->suite->pubkey;
	struct tls_certificate_context context;
	struct x509_certificate cert;
	struct x509_name *name = &cert.subject.name;
	struct x509_public_key *key = &cert.subject.public_key;
	time_t now;
	int rc;

	/* Sanity check */
	if ( end != ( data + len ) ) {
		DBGC ( tls, "TLS %p received overlength Server Certificate\n",
		       tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	/* Parse first certificate and validate certificate chain */
	context.tls = tls;
	context.current = certificate->certificates;
	context.end = end;
	now = time ( NULL );
	if ( ( rc = x509_validate_chain ( tls_parse_next, &context,
					  now, NULL, &cert ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not validate certificate chain: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}

	/* Verify server name */
	if ( ( name->len != strlen ( tls->name ) ) ||
	     ( memcmp ( name->data, tls->name, name->len ) != 0 ) ) {
		DBGC ( tls, "TLS %p server name incorrect\n", tls );
		return -EACCES_WRONG_NAME;
	}

	/* Initialise public key algorithm */
	if ( ( rc = pubkey_init ( pubkey, cipherspec->pubkey_ctx,
				  key->raw.data, key->raw.len ) ) != 0 ) {
		DBGC ( tls, "TLS %p cannot initialise public key: %s\n",
		       tls, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Receive new Certificate Request handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext handshake record
 * @v len		Length of plaintext handshake record
 * @ret rc		Return status code
 */
static int tls_new_certificate_request ( struct tls_session *tls,
					 const void *data __unused,
					 size_t len __unused ) {

	/* We can only send a single certificate, so there is no point
	 * in parsing the Certificate Request.
	 */

	/* Schedule Certificate transmission */
	tls->tx_pending |= TLS_TX_CERTIFICATE;
	tls_tx_resume ( tls );

	return 0;
}

/**
 * Receive new Server Hello Done handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext handshake record
 * @v len		Length of plaintext handshake record
 * @ret rc		Return status code
 */
static int tls_new_server_hello_done ( struct tls_session *tls,
				       const void *data, size_t len ) {
	const struct {
		char next[0];
	} __attribute__ (( packed )) *hello_done = data;
	const void *end = hello_done->next;

	/* Sanity check */
	if ( end != ( data + len ) ) {
		DBGC ( tls, "TLS %p received overlength Server Hello Done\n",
		       tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	/* Schedule Client Key Exchange, Change Cipher, and Finished */
	tls->tx_pending |= ( TLS_TX_CLIENT_KEY_EXCHANGE |
			     TLS_TX_CHANGE_CIPHER |
			     TLS_TX_FINISHED );
	tls_tx_resume ( tls );

	return 0;
}

/**
 * Receive new Finished handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext handshake record
 * @v len		Length of plaintext handshake record
 * @ret rc		Return status code
 */
static int tls_new_finished ( struct tls_session *tls,
			      const void *data, size_t len ) {
	struct digest_algorithm *digest = tls->handshake_digest;
	const struct {
		uint8_t verify_data[12];
		char next[0];
	} __attribute__ (( packed )) *finished = data;
	const void *end = finished->next;
	uint8_t digest_out[ digest->digestsize ];
	uint8_t verify_data[ sizeof ( finished->verify_data ) ];

	/* Sanity check */
	if ( end != ( data + len ) ) {
		DBGC ( tls, "TLS %p received overlength Finished\n", tls );
		DBGC_HD ( tls, data, len );
		return -EINVAL;
	}

	/* Verify data */
	tls_verify_handshake ( tls, digest_out );
	tls_prf_label ( tls, &tls->master_secret, sizeof ( tls->master_secret ),
			verify_data, sizeof ( verify_data ), "server finished",
			digest_out, sizeof ( digest_out ) );
	if ( memcmp ( verify_data, finished->verify_data,
		      sizeof ( verify_data ) ) != 0 ) {
		DBGC ( tls, "TLS %p verification failed\n", tls );
		return -EPERM;
	}

	/* Mark session as ready to transmit plaintext data */
	tls->tx_ready = 1;

	/* Send notification of a window change */
	xfer_window_changed ( &tls->plainstream );

	return 0;
}

/**
 * Receive new Handshake record
 *
 * @v tls		TLS session
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_new_handshake ( struct tls_session *tls,
			       const void *data, size_t len ) {
	const void *end = ( data + len );
	int rc;

	while ( data != end ) {
		const struct {
			uint8_t type;
			uint8_t length[3];
			uint8_t payload[0];
		} __attribute__ (( packed )) *handshake = data;
		void *payload = &handshake->payload;
		size_t payload_len = tls_uint24 ( handshake->length );
		void *next = ( payload + payload_len );

		/* Sanity check */
		if ( next > end ) {
			DBGC ( tls, "TLS %p received overlength Handshake\n",
			       tls );
			DBGC_HD ( tls, data, len );
			return -EINVAL;
		}

		switch ( handshake->type ) {
		case TLS_SERVER_HELLO:
			rc = tls_new_server_hello ( tls, payload, payload_len );
			break;
		case TLS_CERTIFICATE:
			rc = tls_new_certificate ( tls, payload, payload_len );
			break;
		case TLS_CERTIFICATE_REQUEST:
			rc = tls_new_certificate_request ( tls, payload,
							   payload_len );
			break;
		case TLS_SERVER_HELLO_DONE:
			rc = tls_new_server_hello_done ( tls, payload,
							 payload_len );
			break;
		case TLS_FINISHED:
			rc = tls_new_finished ( tls, payload, payload_len );
			break;
		default:
			DBGC ( tls, "TLS %p ignoring handshake type %d\n",
			       tls, handshake->type );
			rc = 0;
			break;
		}

		/* Add to handshake digest (except for Hello Requests,
		 * which are explicitly excluded).
		 */
		if ( handshake->type != TLS_HELLO_REQUEST )
			tls_add_handshake ( tls, data,
					    sizeof ( *handshake ) +
					    payload_len );

		/* Abort on failure */
		if ( rc != 0 )
			return rc;

		/* Move to next handshake record */
		data = next;
	}

	return 0;
}

/**
 * Receive new record
 *
 * @v tls		TLS session
 * @v type		Record type
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_new_record ( struct tls_session *tls, unsigned int type,
			    const void *data, size_t len ) {

	switch ( type ) {
	case TLS_TYPE_CHANGE_CIPHER:
		return tls_new_change_cipher ( tls, data, len );
	case TLS_TYPE_ALERT:
		return tls_new_alert ( tls, data, len );
	case TLS_TYPE_HANDSHAKE:
		return tls_new_handshake ( tls, data, len );
	case TLS_TYPE_DATA:
		return xfer_deliver_raw ( &tls->plainstream, data, len );
	default:
		/* RFC4346 says that we should just ignore unknown
		 * record types.
		 */
		DBGC ( tls, "TLS %p ignoring record type %d\n", tls, type );
		return 0;
	}
}

/******************************************************************************
 *
 * Record encryption/decryption
 *
 ******************************************************************************
 */

/**
 * Calculate HMAC
 *
 * @v tls		TLS session
 * @v cipherspec	Cipher specification
 * @v seq		Sequence number
 * @v tlshdr		TLS header
 * @v data		Data
 * @v len		Length of data
 * @v mac		HMAC to fill in
 */
static void tls_hmac ( struct tls_session *tls __unused,
		       struct tls_cipherspec *cipherspec,
		       uint64_t seq, struct tls_header *tlshdr,
		       const void *data, size_t len, void *hmac ) {
	struct digest_algorithm *digest = cipherspec->suite->digest;
	uint8_t digest_ctx[digest->ctxsize];

	hmac_init ( digest, digest_ctx, cipherspec->mac_secret,
		    &digest->digestsize );
	seq = cpu_to_be64 ( seq );
	hmac_update ( digest, digest_ctx, &seq, sizeof ( seq ) );
	hmac_update ( digest, digest_ctx, tlshdr, sizeof ( *tlshdr ) );
	hmac_update ( digest, digest_ctx, data, len );
	hmac_final ( digest, digest_ctx, cipherspec->mac_secret,
		     &digest->digestsize, hmac );
}

/**
 * Allocate and assemble stream-ciphered record from data and MAC portions
 *
 * @v tls		TLS session
 * @ret data		Data
 * @ret len		Length of data
 * @ret digest		MAC digest
 * @ret plaintext_len	Length of plaintext record
 * @ret plaintext	Allocated plaintext record
 */
static void * __malloc tls_assemble_stream ( struct tls_session *tls,
				    const void *data, size_t len,
				    void *digest, size_t *plaintext_len ) {
	size_t mac_len = tls->tx_cipherspec.suite->digest->digestsize;
	void *plaintext;
	void *content;
	void *mac;

	/* Calculate stream-ciphered struct length */
	*plaintext_len = ( len + mac_len );

	/* Allocate stream-ciphered struct */
	plaintext = malloc ( *plaintext_len );
	if ( ! plaintext )
		return NULL;
	content = plaintext;
	mac = ( content + len );

	/* Fill in stream-ciphered struct */
	memcpy ( content, data, len );
	memcpy ( mac, digest, mac_len );

	return plaintext;
}

/**
 * Allocate and assemble block-ciphered record from data and MAC portions
 *
 * @v tls		TLS session
 * @ret data		Data
 * @ret len		Length of data
 * @ret digest		MAC digest
 * @ret plaintext_len	Length of plaintext record
 * @ret plaintext	Allocated plaintext record
 */
static void * tls_assemble_block ( struct tls_session *tls,
				   const void *data, size_t len,
				   void *digest, size_t *plaintext_len ) {
	size_t blocksize = tls->tx_cipherspec.suite->cipher->blocksize;
	size_t mac_len = tls->tx_cipherspec.suite->digest->digestsize;
	size_t iv_len;
	size_t padding_len;
	void *plaintext;
	void *iv;
	void *content;
	void *mac;
	void *padding;

	/* TLSv1.1 and later use an explicit IV */
	iv_len = ( ( tls->version >= TLS_VERSION_TLS_1_1 ) ? blocksize : 0 );

	/* Calculate block-ciphered struct length */
	padding_len = ( ( blocksize - 1 ) & -( iv_len + len + mac_len + 1 ) );
	*plaintext_len = ( iv_len + len + mac_len + padding_len + 1 );

	/* Allocate block-ciphered struct */
	plaintext = malloc ( *plaintext_len );
	if ( ! plaintext )
		return NULL;
	iv = plaintext;
	content = ( iv + iv_len );
	mac = ( content + len );
	padding = ( mac + mac_len );

	/* Fill in block-ciphered struct */
	tls_generate_random ( tls, iv, iv_len );
	memcpy ( content, data, len );
	memcpy ( mac, digest, mac_len );
	memset ( padding, padding_len, ( padding_len + 1 ) );

	return plaintext;
}

/**
 * Send plaintext record
 *
 * @v tls		TLS session
 * @v type		Record type
 * @v data		Plaintext record
 * @v len		Length of plaintext record
 * @ret rc		Return status code
 */
static int tls_send_plaintext ( struct tls_session *tls, unsigned int type,
				const void *data, size_t len ) {
	struct tls_header plaintext_tlshdr;
	struct tls_header *tlshdr;
	struct tls_cipherspec *cipherspec = &tls->tx_cipherspec;
	struct cipher_algorithm *cipher = cipherspec->suite->cipher;
	void *plaintext = NULL;
	size_t plaintext_len;
	struct io_buffer *ciphertext = NULL;
	size_t ciphertext_len;
	size_t mac_len = cipherspec->suite->digest->digestsize;
	uint8_t mac[mac_len];
	int rc;

	/* Construct header */
	plaintext_tlshdr.type = type;
	plaintext_tlshdr.version = htons ( tls->version );
	plaintext_tlshdr.length = htons ( len );

	/* Calculate MAC */
	tls_hmac ( tls, cipherspec, tls->tx_seq, &plaintext_tlshdr,
		   data, len, mac );

	/* Allocate and assemble plaintext struct */
	if ( is_stream_cipher ( cipher ) ) {
		plaintext = tls_assemble_stream ( tls, data, len, mac,
						  &plaintext_len );
	} else {
		plaintext = tls_assemble_block ( tls, data, len, mac,
						 &plaintext_len );
	}
	if ( ! plaintext ) {
		DBGC ( tls, "TLS %p could not allocate %zd bytes for "
		       "plaintext\n", tls, plaintext_len );
		rc = -ENOMEM;
		goto done;
	}

	DBGC2 ( tls, "Sending plaintext data:\n" );
	DBGC2_HD ( tls, plaintext, plaintext_len );

	/* Allocate ciphertext */
	ciphertext_len = ( sizeof ( *tlshdr ) + plaintext_len );
	ciphertext = xfer_alloc_iob ( &tls->cipherstream, ciphertext_len );
	if ( ! ciphertext ) {
		DBGC ( tls, "TLS %p could not allocate %zd bytes for "
		       "ciphertext\n", tls, ciphertext_len );
		rc = -ENOMEM;
		goto done;
	}

	/* Assemble ciphertext */
	tlshdr = iob_put ( ciphertext, sizeof ( *tlshdr ) );
	tlshdr->type = type;
	tlshdr->version = htons ( tls->version );
	tlshdr->length = htons ( plaintext_len );
	memcpy ( cipherspec->cipher_next_ctx, cipherspec->cipher_ctx,
		 cipher->ctxsize );
	cipher_encrypt ( cipher, cipherspec->cipher_next_ctx, plaintext,
			 iob_put ( ciphertext, plaintext_len ), plaintext_len );

	/* Free plaintext as soon as possible to conserve memory */
	free ( plaintext );
	plaintext = NULL;

	/* Send ciphertext */
	if ( ( rc = xfer_deliver_iob ( &tls->cipherstream,
				       iob_disown ( ciphertext ) ) ) != 0 ) {
		DBGC ( tls, "TLS %p could not deliver ciphertext: %s\n",
		       tls, strerror ( rc ) );
		goto done;
	}

	/* Update TX state machine to next record */
	tls->tx_seq += 1;
	memcpy ( tls->tx_cipherspec.cipher_ctx,
		 tls->tx_cipherspec.cipher_next_ctx, cipher->ctxsize );

 done:
	free ( plaintext );
	free_iob ( ciphertext );
	return rc;
}

/**
 * Split stream-ciphered record into data and MAC portions
 *
 * @v tls		TLS session
 * @v plaintext		Plaintext record
 * @v plaintext_len	Length of record
 * @ret data		Data
 * @ret len		Length of data
 * @ret digest		MAC digest
 * @ret rc		Return status code
 */
static int tls_split_stream ( struct tls_session *tls,
			      void *plaintext, size_t plaintext_len,
			      void **data, size_t *len, void **digest ) {
	void *content;
	size_t content_len;
	void *mac;
	size_t mac_len;

	/* Decompose stream-ciphered data */
	mac_len = tls->rx_cipherspec.suite->digest->digestsize;
	if ( plaintext_len < mac_len ) {
		DBGC ( tls, "TLS %p received underlength record\n", tls );
		DBGC_HD ( tls, plaintext, plaintext_len );
		return -EINVAL;
	}
	content_len = ( plaintext_len - mac_len );
	content = plaintext;
	mac = ( content + content_len );

	/* Fill in return values */
	*data = content;
	*len = content_len;
	*digest = mac;

	return 0;
}

/**
 * Split block-ciphered record into data and MAC portions
 *
 * @v tls		TLS session
 * @v plaintext		Plaintext record
 * @v plaintext_len	Length of record
 * @ret data		Data
 * @ret len		Length of data
 * @ret digest		MAC digest
 * @ret rc		Return status code
 */
static int tls_split_block ( struct tls_session *tls,
			     void *plaintext, size_t plaintext_len,
			     void **data, size_t *len,
			     void **digest ) {
	void *iv;
	size_t iv_len;
	void *content;
	size_t content_len;
	void *mac;
	size_t mac_len;
	void *padding;
	size_t padding_len;
	unsigned int i;

	/* Sanity check */
	if ( plaintext_len < 1 ) {
		DBGC ( tls, "TLS %p received underlength record\n", tls );
		DBGC_HD ( tls, plaintext, plaintext_len );
		return -EINVAL;
	}

	/* TLSv1.1 and later use an explicit IV */
	iv_len = ( ( tls->version >= TLS_VERSION_TLS_1_1 ) ?
		   tls->rx_cipherspec.suite->cipher->blocksize : 0 );

	/* Decompose block-ciphered data */
	mac_len = tls->rx_cipherspec.suite->digest->digestsize;
	padding_len = *( ( uint8_t * ) ( plaintext + plaintext_len - 1 ) );
	if ( plaintext_len < ( iv_len + mac_len + padding_len + 1 ) ) {
		DBGC ( tls, "TLS %p received underlength record\n", tls );
		DBGC_HD ( tls, plaintext, plaintext_len );
		return -EINVAL;
	}
	content_len = ( plaintext_len - iv_len - mac_len - padding_len - 1 );
	iv = plaintext;
	content = ( iv + iv_len );
	mac = ( content + content_len );
	padding = ( mac + mac_len );

	/* Verify padding bytes */
	for ( i = 0 ; i < padding_len ; i++ ) {
		if ( *( ( uint8_t * ) ( padding + i ) ) != padding_len ) {
			DBGC ( tls, "TLS %p received bad padding\n", tls );
			DBGC_HD ( tls, plaintext, plaintext_len );
			return -EINVAL;
		}
	}

	/* Fill in return values */
	*data = content;
	*len = content_len;
	*digest = mac;

	return 0;
}

/**
 * Receive new ciphertext record
 *
 * @v tls		TLS session
 * @v tlshdr		Record header
 * @v ciphertext	Ciphertext record
 * @ret rc		Return status code
 */
static int tls_new_ciphertext ( struct tls_session *tls,
				struct tls_header *tlshdr,
				const void *ciphertext ) {
	struct tls_header plaintext_tlshdr;
	struct tls_cipherspec *cipherspec = &tls->rx_cipherspec;
	struct cipher_algorithm *cipher = cipherspec->suite->cipher;
	size_t record_len = ntohs ( tlshdr->length );
	void *plaintext = NULL;
	void *data;
	size_t len;
	void *mac;
	size_t mac_len = cipherspec->suite->digest->digestsize;
	uint8_t verify_mac[mac_len];
	int rc;

	/* Allocate buffer for plaintext */
	plaintext = malloc ( record_len );
	if ( ! plaintext ) {
		DBGC ( tls, "TLS %p could not allocate %zd bytes for "
		       "decryption buffer\n", tls, record_len );
		rc = -ENOMEM;
		goto done;
	}

	/* Decrypt the record */
	cipher_decrypt ( cipher, cipherspec->cipher_ctx,
			 ciphertext, plaintext, record_len );

	/* Split record into content and MAC */
	if ( is_stream_cipher ( cipher ) ) {
		if ( ( rc = tls_split_stream ( tls, plaintext, record_len,
					       &data, &len, &mac ) ) != 0 )
			goto done;
	} else {
		if ( ( rc = tls_split_block ( tls, plaintext, record_len,
					      &data, &len, &mac ) ) != 0 )
			goto done;
	}

	/* Verify MAC */
	plaintext_tlshdr.type = tlshdr->type;
	plaintext_tlshdr.version = tlshdr->version;
	plaintext_tlshdr.length = htons ( len );
	tls_hmac ( tls, cipherspec, tls->rx_seq, &plaintext_tlshdr,
		   data, len, verify_mac);
	if ( memcmp ( mac, verify_mac, mac_len ) != 0 ) {
		DBGC ( tls, "TLS %p failed MAC verification\n", tls );
		DBGC_HD ( tls, plaintext, record_len );
		goto done;
	}

	DBGC2 ( tls, "Received plaintext data:\n" );
	DBGC2_HD ( tls, data, len );

	/* Process plaintext record */
	if ( ( rc = tls_new_record ( tls, tlshdr->type, data, len ) ) != 0 )
		goto done;

	rc = 0;
 done:
	free ( plaintext );
	return rc;
}

/******************************************************************************
 *
 * Plaintext stream operations
 *
 ******************************************************************************
 */

/**
 * Check flow control window
 *
 * @v tls		TLS session
 * @ret len		Length of window
 */
static size_t tls_plainstream_window ( struct tls_session *tls ) {

	/* Block window unless we are ready to accept data */
	if ( ! tls->tx_ready )
		return 0;

	return xfer_window ( &tls->cipherstream );
}

/**
 * Deliver datagram as raw data
 *
 * @v tls		TLS session
 * @v iobuf		I/O buffer
 * @v meta		Data transfer metadata
 * @ret rc		Return status code
 */
static int tls_plainstream_deliver ( struct tls_session *tls,
				     struct io_buffer *iobuf,
				     struct xfer_metadata *meta __unused ) {
	int rc;
	
	/* Refuse unless we are ready to accept data */
	if ( ! tls->tx_ready ) {
		rc = -ENOTCONN;
		goto done;
	}

	if ( ( rc = tls_send_plaintext ( tls, TLS_TYPE_DATA, iobuf->data,
					 iob_len ( iobuf ) ) ) != 0 )
		goto done;

 done:
	free_iob ( iobuf );
	return rc;
}

/** TLS plaintext stream interface operations */
static struct interface_operation tls_plainstream_ops[] = {
	INTF_OP ( xfer_deliver, struct tls_session *, tls_plainstream_deliver ),
	INTF_OP ( xfer_window, struct tls_session *, tls_plainstream_window ),
	INTF_OP ( intf_close, struct tls_session *, tls_close ),
};

/** TLS plaintext stream interface descriptor */
static struct interface_descriptor tls_plainstream_desc =
	INTF_DESC_PASSTHRU ( struct tls_session, plainstream,
			     tls_plainstream_ops, cipherstream );

/******************************************************************************
 *
 * Ciphertext stream operations
 *
 ******************************************************************************
 */

/**
 * Handle received TLS header
 *
 * @v tls		TLS session
 * @ret rc		Returned status code
 */
static int tls_newdata_process_header ( struct tls_session *tls ) {
	size_t data_len = ntohs ( tls->rx_header.length );

	/* Allocate data buffer now that we know the length */
	assert ( tls->rx_data == NULL );
	tls->rx_data = malloc ( data_len );
	if ( ! tls->rx_data ) {
		DBGC ( tls, "TLS %p could not allocate %zd bytes "
		       "for receive buffer\n", tls, data_len );
		return -ENOMEM;
	}

	/* Move to data state */
	tls->rx_state = TLS_RX_DATA;

	return 0;
}

/**
 * Handle received TLS data payload
 *
 * @v tls		TLS session
 * @ret rc		Returned status code
 */
static int tls_newdata_process_data ( struct tls_session *tls ) {
	int rc;

	/* Process record */
	if ( ( rc = tls_new_ciphertext ( tls, &tls->rx_header,
					 tls->rx_data ) ) != 0 )
		return rc;

	/* Increment RX sequence number */
	tls->rx_seq += 1;

	/* Free data buffer */
	free ( tls->rx_data );
	tls->rx_data = NULL;

	/* Return to header state */
	tls->rx_state = TLS_RX_HEADER;

	return 0;
}

/**
 * Receive new ciphertext
 *
 * @v tls		TLS session
 * @v iobuf		I/O buffer
 * @v meta		Data transfer metadat
 * @ret rc		Return status code
 */
static int tls_cipherstream_deliver ( struct tls_session *tls,
				      struct io_buffer *iobuf,
				      struct xfer_metadata *xfer __unused ) {
	size_t frag_len;
	void *buf;
	size_t buf_len;
	int ( * process ) ( struct tls_session *tls );
	int rc;

	while ( iob_len ( iobuf ) ) {
		/* Select buffer according to current state */
		switch ( tls->rx_state ) {
		case TLS_RX_HEADER:
			buf = &tls->rx_header;
			buf_len = sizeof ( tls->rx_header );
			process = tls_newdata_process_header;
			break;
		case TLS_RX_DATA:
			buf = tls->rx_data;
			buf_len = ntohs ( tls->rx_header.length );
			process = tls_newdata_process_data;
			break;
		default:
			assert ( 0 );
			rc = -EINVAL;
			goto done;
		}

		/* Copy data portion to buffer */
		frag_len = ( buf_len - tls->rx_rcvd );
		if ( frag_len > iob_len  ( iobuf ) )
			frag_len = iob_len ( iobuf );
		memcpy ( ( buf + tls->rx_rcvd ), iobuf->data, frag_len );
		tls->rx_rcvd += frag_len;
		iob_pull ( iobuf, frag_len );

		/* Process data if buffer is now full */
		if ( tls->rx_rcvd == buf_len ) {
			if ( ( rc = process ( tls ) ) != 0 ) {
				tls_close ( tls, rc );
				goto done;
			}
			tls->rx_rcvd = 0;
		}
	}
	rc = 0;

 done:
	free_iob ( iobuf );
	return rc;
}

/** TLS ciphertext stream interface operations */
static struct interface_operation tls_cipherstream_ops[] = {
	INTF_OP ( xfer_deliver, struct tls_session *,
		  tls_cipherstream_deliver ),
	INTF_OP ( xfer_window_changed, struct tls_session *, tls_tx_resume ),
	INTF_OP ( intf_close, struct tls_session *, tls_close ),
};

/** TLS ciphertext stream interface descriptor */
static struct interface_descriptor tls_cipherstream_desc =
	INTF_DESC_PASSTHRU ( struct tls_session, cipherstream,
			     tls_cipherstream_ops, plainstream );

/******************************************************************************
 *
 * Controlling process
 *
 ******************************************************************************
 */

/**
 * TLS TX state machine
 *
 * @v tls		TLS session
 */
static void tls_tx_step ( struct tls_session *tls ) {
	int rc;

	/* Wait for cipherstream to become ready */
	if ( ! xfer_window ( &tls->cipherstream ) )
		return;

	/* Send first pending transmission */
	if ( tls->tx_pending & TLS_TX_CLIENT_HELLO ) {
		/* Send Client Hello */
		if ( ( rc = tls_send_client_hello ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not send Client Hello: %s\n",
			       tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_pending &= ~TLS_TX_CLIENT_HELLO;
	} else if ( tls->tx_pending & TLS_TX_CERTIFICATE ) {
		/* Send Certificate */
		if ( ( rc = tls_send_certificate ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p cold not send Certificate: %s\n",
			       tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_pending &= ~TLS_TX_CERTIFICATE;
	} else if ( tls->tx_pending & TLS_TX_CLIENT_KEY_EXCHANGE ) {
		/* Send Client Key Exchange */
		if ( ( rc = tls_send_client_key_exchange ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not send Client Key "
			       "Exchange: %s\n", tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_pending &= ~TLS_TX_CLIENT_KEY_EXCHANGE;
	} else if ( tls->tx_pending & TLS_TX_CERTIFICATE_VERIFY ) {
		/* Send Certificate Verify */
		if ( ( rc = tls_send_certificate_verify ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not send Certificate "
			       "Verify: %s\n", tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_pending &= ~TLS_TX_CERTIFICATE_VERIFY;
	} else if ( tls->tx_pending & TLS_TX_CHANGE_CIPHER ) {
		/* Send Change Cipher, and then change the cipher in use */
		if ( ( rc = tls_send_change_cipher ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not send Change Cipher: "
			       "%s\n", tls, strerror ( rc ) );
			goto err;
		}
		if ( ( rc = tls_change_cipher ( tls,
						&tls->tx_cipherspec_pending,
						&tls->tx_cipherspec )) != 0 ){
			DBGC ( tls, "TLS %p could not activate TX cipher: "
			       "%s\n", tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_seq = 0;
		tls->tx_pending &= ~TLS_TX_CHANGE_CIPHER;
	} else if ( tls->tx_pending & TLS_TX_FINISHED ) {
		/* Send Finished */
		if ( ( rc = tls_send_finished ( tls ) ) != 0 ) {
			DBGC ( tls, "TLS %p could not send Finished: %s\n",
			       tls, strerror ( rc ) );
			goto err;
		}
		tls->tx_pending &= ~TLS_TX_FINISHED;
	}

	/* Reschedule process if pending transmissions remain */
	if ( tls->tx_pending )
		tls_tx_resume ( tls );

	return;

 err:
	tls_close ( tls, rc );
}

/** TLS TX process descriptor */
static struct process_descriptor tls_process_desc =
	PROC_DESC_ONCE ( struct tls_session, process, tls_tx_step );

/******************************************************************************
 *
 * Instantiator
 *
 ******************************************************************************
 */

int add_tls ( struct interface *xfer, const char *name,
	      struct interface **next ) {
	struct tls_session *tls;
	int rc;

	/* Allocate and initialise TLS structure */
	tls = malloc ( sizeof ( *tls ) );
	if ( ! tls ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	memset ( tls, 0, sizeof ( *tls ) );
	ref_init ( &tls->refcnt, free_tls );
	tls->name = name;
	intf_init ( &tls->plainstream, &tls_plainstream_desc, &tls->refcnt );
	intf_init ( &tls->cipherstream, &tls_cipherstream_desc, &tls->refcnt );
	tls->version = TLS_VERSION_TLS_1_2;
	tls_clear_cipher ( tls, &tls->tx_cipherspec );
	tls_clear_cipher ( tls, &tls->tx_cipherspec_pending );
	tls_clear_cipher ( tls, &tls->rx_cipherspec );
	tls_clear_cipher ( tls, &tls->rx_cipherspec_pending );
	tls->client_random.gmt_unix_time = time ( NULL );
	if ( ( rc = tls_generate_random ( tls, &tls->client_random.random,
			  ( sizeof ( tls->client_random.random ) ) ) ) != 0 ) {
		goto err_random;
	}
	tls->pre_master_secret.version = htons ( tls->version );
	if ( ( rc = tls_generate_random ( tls, &tls->pre_master_secret.random,
		      ( sizeof ( tls->pre_master_secret.random ) ) ) ) != 0 ) {
		goto err_random;
	}
	digest_init ( &md5_sha1_algorithm, tls->handshake_md5_sha1_ctx );
	digest_init ( &sha256_algorithm, tls->handshake_sha256_ctx );
	tls->handshake_digest = &sha256_algorithm;
	tls->handshake_ctx = tls->handshake_sha256_ctx;
	tls->tx_pending = TLS_TX_CLIENT_HELLO;
	process_init ( &tls->process, &tls_process_desc, &tls->refcnt );

	/* Attach to parent interface, mortalise self, and return */
	intf_plug_plug ( &tls->plainstream, xfer );
	*next = &tls->cipherstream;
	ref_put ( &tls->refcnt );
	return 0;

 err_random:
	ref_put ( &tls->refcnt );
 err_alloc:
	return rc;
}
