/* decode.c - ber input decoding routines */
/*
 * Copyright 1998-1999 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* Portions
 * Copyright (c) 1990 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include "portable.h"

#include <stdio.h>
#include <stdlib.h>

#include <ac/stdarg.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "lber-int.h"

static int ber_getnint LDAP_P(( BerElement *ber, long *num, int len ));

/* return the tag - LBER_DEFAULT returned means trouble */
unsigned long
ber_get_tag( BerElement *ber )
{
	unsigned char	xbyte;
	unsigned long	tag;
	char		*tagp;
	unsigned int	i;

	assert( ber != NULL );

	if ( ber_read( ber, (char *) &xbyte, 1 ) != 1 )
		return( LBER_DEFAULT );

	if ( (xbyte & LBER_BIG_TAG_MASK) != LBER_BIG_TAG_MASK )
		return( (unsigned long) xbyte );

	tagp = (char *) &tag;
	tagp[0] = xbyte;
	for ( i = 1; i < sizeof(long); i++ ) {
		if ( ber_read( ber, (char *) &xbyte, 1 ) != 1 )
			return( LBER_DEFAULT );

		tagp[i] = xbyte;

		if ( ! (xbyte & LBER_MORE_TAG_MASK) )
			break;
	}

	/* tag too big! */
	if ( i == sizeof(long) )
		return( LBER_DEFAULT );

	/* want leading, not trailing 0's */
	return( tag >> (sizeof(long) - i - 1) );
}

unsigned long
ber_skip_tag( BerElement *ber, unsigned long *len )
{
	unsigned long	tag;
	unsigned char	lc;
	unsigned int	noctets;
	int		diff;
	unsigned long	netlen;

	assert( ber != NULL );
	assert( len != NULL );

	/*
	 * Any ber element looks like this: tag length contents.
	 * Assuming everything's ok, we return the tag byte (we
	 * can assume a single byte), and return the length in len.
	 *
	 * Assumptions:
	 *	1) definite lengths
	 *	2) primitive encodings used whenever possible
	 */

	/*
	 * First, we read the tag.
	 */

	if ( (tag = ber_get_tag( ber )) == LBER_DEFAULT )
		return( LBER_DEFAULT );

	/*
	 * Next, read the length.  The first byte contains the length of
	 * the length.  If bit 8 is set, the length is the long form,
	 * otherwise it's the short form.  We don't allow a length that's
	 * greater than what we can hold in an unsigned long.
	 */

	*len = netlen = 0;
	if ( ber_read( ber, (char *) &lc, 1 ) != 1 )
		return( LBER_DEFAULT );
	if ( lc & 0x80U ) {
		noctets = (lc & 0x7fU);
		if ( (unsigned) noctets > sizeof(unsigned long) )
			return( LBER_DEFAULT );
		diff = sizeof(unsigned long) - noctets;
		if ( (unsigned) ber_read( ber, (char *) &netlen + diff, noctets )
		    != noctets )
			return( LBER_DEFAULT );
		*len = AC_NTOHL( netlen );
	} else {
		*len = lc;
	}

	return( tag );
}

unsigned long
ber_peek_tag(
	LDAP_CONST BerElement *ber_in, /* not const per c-api-02 */
	unsigned long *len )
{
	unsigned long	tag;
	BerElement *ber = ber_dup( ber_in );

	if( ber == NULL ) {
		return LBER_ERROR;
	}

	tag = ber_skip_tag( ber, len );

	ber_free( ber, 1 );
	return( tag );
}

static int
ber_getnint( BerElement *ber, long *num, int len )
{
	int	diff, sign, i;
	long	netnum;
	char    *p;

	assert( ber != NULL );
	assert( num != NULL );

	/*
	 * The tag and length have already been stripped off.  We should
	 * be sitting right before len bytes of 2's complement integer,
	 * ready to be read straight into an int.  We may have to sign
	 * extend after we read it in.
	 */

	if ( (unsigned) len > sizeof(long) )
		return( -1 );

	netnum = 0;
	diff = sizeof(long) - len;
	/* read into the low-order bytes of netnum */
	if ( ber_read( ber, ((char *) &netnum) + diff, len ) != len )
		return( -1 );

        /* sign extend if necessary */
        p = (char *) &netnum;
        sign = (0x80 & *(p+diff) );
        if ( sign && ((unsigned) len < sizeof(long)) ) {
                for ( i = 0; i < diff; i++ ) {
                        *(p+i) = (unsigned char) 0xff;
		}
	}
	*num = AC_NTOHL( netnum );

	return( len );
}

unsigned long
ber_get_int( BerElement *ber, long *num )
{
	unsigned long	tag, len;

	if ( (tag = ber_skip_tag( ber, &len )) == LBER_DEFAULT )
		return( LBER_DEFAULT );

	if ( (unsigned long) ber_getnint( ber, num, (int)len ) != len )
		return( LBER_DEFAULT );
	else
		return( tag );
}

unsigned long
ber_get_stringb( BerElement *ber, char *buf, unsigned long *len )
{
	unsigned long	datalen, tag;
#ifdef STR_TRANSLATION
	char		*transbuf;
#endif /* STR_TRANSLATION */

	if ( (tag = ber_skip_tag( ber, &datalen )) == LBER_DEFAULT )
		return( LBER_DEFAULT );
	if ( datalen > (*len - 1) )
		return( LBER_DEFAULT );

	if ( (unsigned long) ber_read( ber, buf, datalen ) != datalen )
		return( LBER_DEFAULT );

	buf[datalen] = '\0';

#ifdef STR_TRANSLATION
	if ( datalen > 0 && ( ber->ber_options & LBER_TRANSLATE_STRINGS ) != 0
	    && ber->ber_decode_translate_proc ) {
		transbuf = buf;
		++datalen;
		if ( (*(ber->ber_decode_translate_proc))( &transbuf, &datalen,
		    0 ) != 0 ) {
			return( LBER_DEFAULT );
		}
		if ( datalen > *len ) {
			free( transbuf );
			return( LBER_DEFAULT );
		}
		SAFEMEMCPY( buf, transbuf, datalen );
		free( transbuf );
		--datalen;
	}
#endif /* STR_TRANSLATION */

	*len = datalen;
	return( tag );
}

unsigned long
ber_get_stringa( BerElement *ber, char **buf )
{
	unsigned long	datalen, tag;

	if ( (tag = ber_skip_tag( ber, &datalen )) == LBER_DEFAULT ) {
		*buf = NULL;
		return( LBER_DEFAULT );
	}

	if ( (*buf = (char *) malloc( (size_t)datalen + 1 )) == NULL )
		return( LBER_DEFAULT );

	if ( (unsigned long) ber_read( ber, *buf, datalen ) != datalen ) {
		free( *buf );
		*buf = NULL;
		return( LBER_DEFAULT );
	}
	(*buf)[datalen] = '\0';

#ifdef STR_TRANSLATION
	if ( datalen > 0 && ( ber->ber_options & LBER_TRANSLATE_STRINGS ) != 0
	    && ber->ber_decode_translate_proc ) {
		++datalen;
		if ( (*(ber->ber_decode_translate_proc))( buf, &datalen, 1 )
		    != 0 ) {
			free( *buf );
			*buf = NULL;
			return( LBER_DEFAULT );
		}
	}
#endif /* STR_TRANSLATION */

	return( tag );
}

unsigned long
ber_get_stringal( BerElement *ber, struct berval **bv )
{
	unsigned long	len, tag;

	if ( (tag = ber_skip_tag( ber, &len )) == LBER_DEFAULT ) {
		*bv = NULL;
		return( LBER_DEFAULT );
	}

	if ( (*bv = (struct berval *) malloc( sizeof(struct berval) )) == NULL )
		return( LBER_DEFAULT );

	if ( ((*bv)->bv_val = (char *) malloc( (size_t)len + 1 )) == NULL ) {
		free( *bv );
		*bv = NULL;
		return( LBER_DEFAULT );
	}

	if ( (unsigned long) ber_read( ber, (*bv)->bv_val, len ) != len ) {
		ber_bvfree( *bv );
		*bv = NULL;
		return( LBER_DEFAULT );
	}
	((*bv)->bv_val)[len] = '\0';
	(*bv)->bv_len = len;

#ifdef STR_TRANSLATION
	if ( len > 0 && ( ber->ber_options & LBER_TRANSLATE_STRINGS ) != 0
	    && ber->ber_decode_translate_proc ) {
		++len;
		if ( (*(ber->ber_decode_translate_proc))( &((*bv)->bv_val),
		    &len, 1 ) != 0 ) {
			ber_bvfree( *bv );
			*bv = NULL;
			return( LBER_DEFAULT );
		}
		(*bv)->bv_len = len - 1;
	}
#endif /* STR_TRANSLATION */

	return( tag );
}

unsigned long
ber_get_bitstringa( BerElement *ber, char **buf, unsigned long *blen )
{
	unsigned long	datalen, tag;
	unsigned char	unusedbits;

	if ( (tag = ber_skip_tag( ber, &datalen )) == LBER_DEFAULT ) {
		*buf = NULL;
		return( LBER_DEFAULT );
	}
	--datalen;

	if ( (*buf = (char *) malloc( (size_t)datalen )) == NULL )
		return( LBER_DEFAULT );

	if ( ber_read( ber, (char *)&unusedbits, 1 ) != 1 ) {
		free( buf );
		*buf = NULL;
		return( LBER_DEFAULT );
	}

	if ( (unsigned long) ber_read( ber, *buf, datalen ) != datalen ) {
		free( buf );
		*buf = NULL;
		return( LBER_DEFAULT );
	}

	*blen = datalen * 8 - unusedbits;
	return( tag );
}

unsigned long
ber_get_null( BerElement *ber )
{
	unsigned long	len, tag;

	if ( (tag = ber_skip_tag( ber, &len )) == LBER_DEFAULT )
		return( LBER_DEFAULT );

	if ( len != 0 )
		return( LBER_DEFAULT );

	return( tag );
}

unsigned long
ber_get_boolean( BerElement *ber, int *boolval )
{
	long	longbool;
	int	rc;

	rc = ber_get_int( ber, &longbool );
	*boolval = longbool;

	return( rc );
}

unsigned long
ber_first_element( BerElement *ber, unsigned long *len, char **last )
{
	/* skip the sequence header, use the len to mark where to stop */
	if ( ber_skip_tag( ber, len ) == LBER_DEFAULT ) {
		*last = NULL;
		return( LBER_DEFAULT );
	}

	*last = ber->ber_ptr + *len;

	if ( *last == ber->ber_ptr ) {
#ifdef LBER_END_SEQORSET 
		return( LBER_END_SEQORSET );
#else
		return( LBER_DEFAULT );
#endif
	}

	return( ber_peek_tag( ber, len ) );
}

unsigned long
ber_next_element( BerElement *ber, unsigned long *len, char *last )
{
	if ( ber->ber_ptr == last ) {
#ifdef LBER_END_SEQORSET 
		return( LBER_END_SEQORSET );
#else
		return( LBER_DEFAULT );
#endif
	}

	return( ber_peek_tag( ber, len ) );
}

/* VARARGS */
unsigned long
ber_scanf
#if HAVE_STDARG
	( BerElement *ber,
	LDAP_CONST char *fmt,
	... )
#else
	( va_alist )
va_dcl
#endif
{
	va_list		ap;
#ifndef HAVE_STDARG
	BerElement	*ber;
	char		*fmt;
#endif
	LDAP_CONST char		*fmt_reset;
	char		*last;
	char		*s, **ss, ***sss;
	struct berval 	***bv, **bvp, *bval;
	int		*i, j;
	long		*l;
	unsigned long	rc, tag, len;

	assert( ber != NULL );

#ifdef HAVE_STDARG
	va_start( ap, fmt );
#else
	va_start( ap );
	ber = va_arg( ap, BerElement * );
	fmt = va_arg( ap, char * );
#endif

	assert( ber != NULL );
	assert( fmt != NULL );

	fmt_reset = fmt;

	ber_log_printf( LDAP_DEBUG_TRACE, ber->ber_debug,
		"ber_scanf fmt (%s) ber:\n", fmt );
	ber_log_dump( LDAP_DEBUG_BER, ber->ber_debug, ber, 1 );

	for ( rc = 0; *fmt && rc != LBER_DEFAULT; fmt++ ) {
		/* When this is modified, remember to update
		 * the error-cleanup code below accordingly. */
		switch ( *fmt ) {
		case '!': { /* Hook */
				BERDecodeCallback *f;
				void *p;

				f = va_arg( ap, BERDecodeCallback * );
				p = va_arg( ap, void * );

				rc = (*f)( ber, p, 0 );
			} break;

		case 'a':	/* octet string - allocate storage as needed */
			ss = va_arg( ap, char ** );
			rc = ber_get_stringa( ber, ss );
			break;

		case 'b':	/* boolean */
			i = va_arg( ap, int * );
			rc = ber_get_boolean( ber, i );
			break;

		case 'e':	/* enumerated */
		case 'i':	/* int */
			l = va_arg( ap, long * );
			rc = ber_get_int( ber, l );
			break;

		case 'l':	/* length of next item */
			l = va_arg( ap, long * );
			rc = ber_peek_tag( ber, l );
			break;

		case 'n':	/* null */
			rc = ber_get_null( ber );
			break;

		case 's':	/* octet string - in a buffer */
			s = va_arg( ap, char * );
			l = va_arg( ap, long * );
			rc = ber_get_stringb( ber, s, l );
			break;

		case 'o':	/* octet string in a supplied berval */
			bval = va_arg( ap, struct berval * );
			ber_peek_tag( ber, &bval->bv_len );
			rc = ber_get_stringa( ber, &bval->bv_val );
			break;

		case 'O':	/* octet string - allocate & include length */
			bvp = va_arg( ap, struct berval ** );
			rc = ber_get_stringal( ber, bvp );
			break;

		case 'B':	/* bit string - allocate storage as needed */
			ss = va_arg( ap, char ** );
			l = va_arg( ap, long * ); /* for length, in bits */
			rc = ber_get_bitstringa( ber, ss, l );
			break;

		case 't':	/* tag of next item */
			l = va_arg( ap, long * );
			*l = rc = ber_peek_tag( ber, &len );
			break;

		case 'T':	/* skip tag of next item */
			l = va_arg( ap, long * );
			*l = rc = ber_skip_tag( ber, &len );
			break;

		case 'v':	/* sequence of strings */
			sss = va_arg( ap, char *** );
			*sss = NULL;
			j = 0;
			for ( tag = ber_first_element( ber, &len, &last );
			    tag != LBER_DEFAULT && 
#ifdef LBER_END_SEQORSET
					tag != LBER_END_SEQORSET &&
#endif
					rc != LBER_DEFAULT;
			    tag = ber_next_element( ber, &len, last ) )
			{
				if ( *sss == NULL ) {
					*sss = (char **) malloc(
					    2 * sizeof(char *) );
				} else {
					*sss = (char **) realloc( *sss,
					    (j + 2) * sizeof(char *) );
				}
				rc = ber_get_stringa( ber, &((*sss)[j]) );
				j++;
			}
#ifdef LBER_END_SEQORSET
			if (rc != LBER_DEFAULT && 
				tag != LBER_END_SEQORSET )
			{
				rc = LBER_DEFAULT;
			}
#endif
			if ( j > 0 )
				(*sss)[j] = NULL;
			break;

		case 'V':	/* sequence of strings + lengths */
			bv = va_arg( ap, struct berval *** );
			*bv = NULL;
			j = 0;
			for ( tag = ber_first_element( ber, &len, &last );
			    tag != LBER_DEFAULT && 
#ifdef LBER_END_SEQORSET
					tag != LBER_END_SEQORSET &&
#endif
					rc != LBER_DEFAULT;
			    tag = ber_next_element( ber, &len, last ) )
			{
				if ( *bv == NULL ) {
					*bv = (struct berval **) malloc(
					    2 * sizeof(struct berval *) );
				} else {
					*bv = (struct berval **) realloc( *bv,
					    (j + 2) * sizeof(struct berval *) );
				}
				rc = ber_get_stringal( ber, &((*bv)[j]) );
				j++;
			}
#ifdef LBER_END_SEQORSET
			if (rc != LBER_DEFAULT && 
				tag != LBER_END_SEQORSET )
			{
				rc = LBER_DEFAULT;
			}
#endif
			if ( j > 0 )
				(*bv)[j] = NULL;
			break;

		case 'x':	/* skip the next element - whatever it is */
			if ( (rc = ber_skip_tag( ber, &len )) == LBER_DEFAULT )
				break;
			ber->ber_ptr += len;
			break;

		case '{':	/* begin sequence */
		case '[':	/* begin set */
			if ( *(fmt + 1) != 'v' && *(fmt + 1) != 'V' )
				rc = ber_skip_tag( ber, &len );
			break;

		case '}':	/* end sequence */
		case ']':	/* end set */
			break;

		default:
			if( ber->ber_debug ) {
				ber_log_printf( LDAP_DEBUG_ANY, ber->ber_debug,
					"ber_scanf: unknown fmt %c\n", *fmt );
			}
			rc = LBER_DEFAULT;
			break;
		}
	}

	va_end( ap );

	if ( rc == LBER_DEFAULT ) {
	    /*
	     * Error.  Reclaim malloced memory that was given to the caller.
	     * Set allocated pointers to NULL, "data length" outvalues to 0.
	     */
#ifdef HAVE_STDARG
	    va_start( ap, fmt );
#else
	    va_start( ap );
	    (void) va_arg( ap, BerElement * );
	    (void) va_arg( ap, char * );
#endif

	    for ( ; fmt_reset < fmt; fmt_reset++ ) {
		switch ( *fmt_reset ) {
		case '!': { /* Hook */
				BERDecodeCallback *f;
				void *p;

				f = va_arg( ap, BERDecodeCallback * );
				p = va_arg( ap, void * );

				(void) (*f)( ber, p, 1 );
			} break;

		case 'a':	/* octet string - allocate storage as needed */
			ss = va_arg( ap, char ** );
			if ( *ss ) {
				free( *ss );
				*ss = NULL;
			}
			break;

		case 'b':	/* boolean */
			(void) va_arg( ap, int * );
			break;

		case 's':	/* octet string - in a buffer */
			(void) va_arg( ap, char * );
			(void) va_arg( ap, long * );
			break;

		case 'e':	/* enumerated */
		case 'i':	/* int */
		case 'l':	/* length of next item */
		case 't':	/* tag of next item */
		case 'T':	/* skip tag of next item */
			(void) va_arg( ap, long * );
			break;

		case 'o':	/* octet string in a supplied berval */
			bval = va_arg( ap, struct berval * );
			if ( bval->bv_val != NULL ) {
				free( bval->bv_val );
				bval->bv_val = NULL;
			}
			bval->bv_len = 0;
			break;

		case 'O':	/* octet string - allocate & include length */
			bvp = va_arg( ap, struct berval ** );
			if ( *bvp ) {
				ber_bvfree( *bvp );
				*bvp = NULL;
			}
			break;

		case 'B':	/* bit string - allocate storage as needed */
			ss = va_arg( ap, char ** );
			if ( *ss ) {
				free( *ss );
				*ss = NULL;
			}
			*(va_arg( ap, long * )) = 0; /* for length, in bits */
			break;

		case 'v':	/* sequence of strings */
			sss = va_arg( ap, char *** );
			if ( *sss ) {
				for (j = 0;  (*sss)[j];  j++) {
					free( (*sss)[j] );
					(*sss)[j] = NULL;
				}
				free( *sss );
				*sss = NULL;
			}
			break;

		case 'V':	/* sequence of strings + lengths */
			bv = va_arg( ap, struct berval *** );
			if ( *bv ) {
				ber_bvecfree( *bv );
				*bv = NULL;
			}
			break;

		case 'n':	/* null */
		case 'x':	/* skip the next element - whatever it is */
		case '{':	/* begin sequence */
		case '[':	/* begin set */
		case '}':	/* end sequence */
		case ']':	/* end set */
			break;

		default:
			/* format should be good */
			assert( 0 );
		}
	    }

	    va_end( ap );
	}

	return( rc );
}

void
ber_bvfree( struct berval *bv )
{
	assert(bv != NULL);			/* bv damn better point to something */

	if ( bv->bv_val != NULL )
		free( bv->bv_val );
	free( (char *) bv );
}

void
ber_bvecfree( struct berval **bv )
{
	int	i;

	assert(bv != NULL);			/* bv damn better point to something */

	for ( i = 0; bv[i] != NULL; i++ )
		ber_bvfree( bv[i] );
	free( (char *) bv );
}

struct berval *
ber_bvdup(
	LDAP_CONST struct berval *bv )
{
	struct berval	*new;

	assert( bv != NULL );

	if( bv == NULL ) {
		return NULL;
	}

	if ( (new = (struct berval *) malloc( sizeof(struct berval) ))
	    == NULL ) {
		return( NULL );
	}

	if ( bv->bv_val == NULL ) {
		new->bv_val = NULL;
		new->bv_len = 0;
		return ( new );
	}

	if ( (new->bv_val = (char *) malloc( bv->bv_len + 1 )) == NULL ) {
		free( new );
		return( NULL );
	}

	SAFEMEMCPY( new->bv_val, bv->bv_val, (size_t) bv->bv_len );
	new->bv_val[bv->bv_len] = '\0';
	new->bv_len = bv->bv_len;

	return( new );
}


#ifdef STR_TRANSLATION
void
ber_set_string_translators( BerElement *ber, BERTranslateProc encode_proc,
	BERTranslateProc decode_proc )
{
	assert( ber != NULL );

    ber->ber_encode_translate_proc = encode_proc;
    ber->ber_decode_translate_proc = decode_proc;
}
#endif /* STR_TRANSLATION */
