//==========================================================================
//
//      include/netinet/icmp_var.h
//
//==========================================================================
//####BSDCOPYRIGHTBEGIN####
//
// -------------------------------------------
//
// Portions of this software may have been derived from OpenBSD, 
// FreeBSD or other sources, and are covered by the appropriate
// copyright disclaimers included herein.
//
// Portions created by Red Hat are
// Copyright (C) 2002 Red Hat, Inc. All Rights Reserved.
//
// -------------------------------------------
//
//####BSDCOPYRIGHTEND####
//==========================================================================

/*
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)icmp_var.h	8.1 (Berkeley) 6/10/93
 * $FreeBSD: src/sys/netinet/icmp_var.h,v 1.15.2.1 2001/02/24 21:35:18 bmilekic Exp $
 */

#ifndef _NETINET_ICMP_VAR_H_
#define _NETINET_ICMP_VAR_H_

/*
 * Variables related to this implementation
 * of the internet control message protocol.
 */
struct	icmpstat {
/* statistics related to icmp packets generated */
	u_long	icps_error;		/* # of calls to icmp_error */
	u_long	icps_oldshort;		/* no error 'cuz old ip too short */
	u_long	icps_oldicmp;		/* no error 'cuz old was icmp */
	u_long	icps_outhist[ICMP_MAXTYPE + 1];
/* statistics related to input messages processed */
 	u_long	icps_badcode;		/* icmp_code out of range */
	u_long	icps_tooshort;		/* packet < ICMP_MINLEN */
	u_long	icps_checksum;		/* bad checksum */
	u_long	icps_badlen;		/* calculated bound mismatch */
	u_long	icps_reflect;		/* number of responses */
	u_long	icps_inhist[ICMP_MAXTYPE + 1];
	u_long	icps_bmcastecho; 	/* b/mcast echo requests dropped */
	u_long	icps_bmcasttstamp; 	/* b/mcast tstamp requests dropped */
};

#ifdef _KERNEL
    #ifdef ICMP_BANDLIM
        extern int badport_bandlim __P((int));
    #endif
    #define BANDLIM_UNLIMITED -1
    #define BANDLIM_ICMP_UNREACH 0
    #define BANDLIM_ICMP_ECHO 1
    #define BANDLIM_ICMP_TSTAMP 2
    #define BANDLIM_RST_CLOSEDPORT 3 /* No connection, and no listeners */
    #define BANDLIM_RST_OPENPORT 4   /* No connection, listener */
    #define BANDLIM_MAX 4
    
    // Used by SNMP
    extern struct icmpstat icmpstat;

#endif

#endif
