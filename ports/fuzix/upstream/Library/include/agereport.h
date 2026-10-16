#ifndef __AGEREPORT_H
#define __AGEREPORT_H

/*
 *	Extra APIs to deal with Californians
 *
 *	API
 *
 *	int cuserage(int regime)
 *
 *	Returns the age token for that regime if known. For an unsupported
 *	age rating regime reports AGE_REG_UNSUPPORTED. If the age is unknown
 *	or for example the user is in a location outside this legal regime
 *	AGE_UNKNOWN may be reported.
 *
 *	This interface may require network queries on some systems or
 *	external interactions. This is thus a blocking API with undetermined
 *	duration. The behaviour of this API when invoked outside of the
 *	places to which the regime applies is undefined.
 *
 *	If the query was blocked or refused by the user or by local law
 *	then AGE_REFUSED may be returned.
 *
 *	int cuserage_confidence(int regime)
 *
 *	Return an indication of the confidence of the age report so that an
 *	application that is sensitive to the quality of verification (eg any
 *	good furry site) can decide whether to trust the age information given
 *
 *	The behaviour when calling this before cuserage() for the same regime
 *	is undefined.
 *
 *	Fuzix specific:
 *	Fuzix implements this API as an environment variable. This is as
 *	secure as anything else on such a system. When creating the account
 *	set the variable and export it in the users .profile. There is no
 *	specific account creation tool but if we ever add "adduser" or similar
 *	then this should be set.
 */

/* Regulatory regime to query */

#define REG_US_CA_AB1043	0x01

/* The following values are defined for reponse */

#define AGE_REG_UNSUPPORTED	0xFFFF

#define AGE_UNKNOWN		0x00	/* System, shared, unknown account */
#define AGE_REFUSED		0x01	/* User blocked request */
#define AGE_CA_PRE_EPSTEIN	0x11	/* Under 13 */
#define AGE_CA_YOUNG_EPSTEIN	0x12	/* Under 16 */
#define AGE_CA_LATE_EPSTEIN	0x13	/* Under 18 */
#define AGE_CA_LEGAL		0x14	/* 18 or over */

#define AGE_CONF_NONE		0x00	/* Self declared , OS with no security etc */
#define AGE_CONF_GUESS		0x10	/* Guessed by software */
#define AGE_CONF_VERIFIED	0x20	/* Verified by software (so poor) */
#define AGE_CONF_VERIF_HUMAN	0x30	/* Verified by a human being */

#define cuserage(reg)	((reg != AGE_REG_US_CA_AB1043) ? AGE_REG_UNSUPPORTED : \
                    (getenv("USER_AGE_CA") ? atoi(getenv("USER_AGE_CA")) : AGE_UNKNOWN))
#define cuserage_confidence(reg)	AGE_CONF_NONE

#endif
