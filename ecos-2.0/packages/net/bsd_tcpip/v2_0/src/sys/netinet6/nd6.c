//==========================================================================
//
//      src/sys/netinet6/nd6.c
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

/*	$KAME: nd6.c,v 1.221 2001/12/18 02:23:45 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * XXX
 * KAME 970409 note:
 * BSD/OS version heavily modifies this code, related to llinfo.
 * Since we don't have BSD/OS version of net/route.c in our hand,
 * I left the code mostly as it was in 970310.  -- itojun
 */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/protosw.h>
#include <sys/errno.h>
#include <sys/queue.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>

#include <netinet/in.h>
#ifndef __NetBSD__
#include <netinet/if_ether.h>
#ifdef __bsdi__
#include <net/if_fddi.h>
#endif
#ifdef __OpenBSD__
#include <netinet/ip_ipsp.h>
#endif
#else /* __NetBSD__ */
#include <net/if_ether.h>
#include <netinet/if_inarp.h>
#include <net/if_fddi.h>
#endif /* __NetBSD__ */
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/nd6.h>
#include <netinet/icmp6.h>

#if defined(__NetBSD__)
extern struct ifnet loif[NLOOP];
#endif

#define ND6_SLOWTIMER_INTERVAL (60 * 60) /* 1 hour */
#define ND6_RECALC_REACHTM_INTERVAL (60 * 120) /* 2 hours */

#define SIN6(s) ((struct sockaddr_in6 *)s)
#define SDL(s) ((struct sockaddr_dl *)s)

/* timer values */
int	nd6_prune	= 1;	/* walk list every 1 seconds */
int	nd6_delay	= 5;	/* delay first probe time 5 second */
int	nd6_umaxtries	= 3;	/* maximum unicast query */
int	nd6_mmaxtries	= 3;	/* maximum multicast query */
int	nd6_useloopback = 1;	/* use loopback interface for local traffic */
int	nd6_gctimer	= (60 * 60 * 24); /* 1 day: garbage collection timer */

/* preventing too many loops in ND option parsing */
int nd6_maxndopt = 10;	/* max # of ND options allowed */

int nd6_maxnudhint = 0;	/* max # of subsequent upper layer hints */

int nd6_debug = 1;

/* for debugging? */
static int nd6_inuse, nd6_allocated;

struct llinfo_nd6 llinfo_nd6 = {&llinfo_nd6, &llinfo_nd6};
static size_t nd_ifinfo_indexlim = 8;
struct nd_ifinfo *nd_ifinfo = NULL;
struct nd_drhead nd_defrouter;
struct nd_prhead nd_prefix = { 0 };

int nd6_recalc_reachtm_interval = ND6_RECALC_REACHTM_INTERVAL;
static struct sockaddr_in6 all1_sa;

static void nd6_slowtimo __P((void *));
static int regen_tmpaddr __P((struct in6_ifaddr *));
static struct llinfo_nd6 *nd6_free __P((struct rtentry *, int));

#ifdef __NetBSD__
struct callout nd6_slowtimo_ch = CALLOUT_INITIALIZER;
struct callout nd6_timer_ch = CALLOUT_INITIALIZER;
extern struct callout in6_tmpaddrtimer_ch;
#elif (defined(__FreeBSD__) && __FreeBSD__ >= 3)
struct callout nd6_slowtimo_ch;
struct callout nd6_timer_ch;
extern struct callout in6_tmpaddrtimer_ch;
#elif defined(__OpenBSD__)
struct timeout nd6_slowtimo_ch;
struct timeout nd6_timer_ch;
extern struct timeout in6_tmpaddrtimer_ch;
#endif

void
nd6_init()
{
	static int nd6_init_done = 0;
	int i;

	if (nd6_init_done) {
		log(LOG_NOTICE, "nd6_init called more than once(ignored)\n");
		return;
	}

	all1_sa.sin6_family = AF_INET6;
	all1_sa.sin6_len = sizeof(struct sockaddr_in6);
	for (i = 0; i < sizeof(all1_sa.sin6_addr); i++)
		all1_sa.sin6_addr.s6_addr[i] = 0xff;

	/* initialization of the default router list */
	TAILQ_INIT(&nd_defrouter);

	nd6_init_done = 1;

	/* start timer */
#if defined(__NetBSD__) || (defined(__FreeBSD__) && __FreeBSD__ >= 3)
	callout_reset(&nd6_slowtimo_ch, ND6_SLOWTIMER_INTERVAL * hz,
	    nd6_slowtimo, NULL);
#elif defined(__OpenBSD__)
	timeout_set(&nd6_slowtimo_ch, nd6_slowtimo, NULL);
	timeout_add(&nd6_slowtimo_ch, ND6_SLOWTIMER_INTERVAL * hz);
#else
	timeout(nd6_slowtimo, (caddr_t)0, ND6_SLOWTIMER_INTERVAL * hz);
#endif
}

void
nd6_ifattach(ifp)
	struct ifnet *ifp;
{

	/*
	 * We have some arrays that should be indexed by if_index.
	 * since if_index will grow dynamically, they should grow too.
	 */
	if (nd_ifinfo == NULL || if_index >= nd_ifinfo_indexlim) {
		size_t n;
		caddr_t q;

		while (if_index >= nd_ifinfo_indexlim)
			nd_ifinfo_indexlim <<= 1;

		/* grow nd_ifinfo */
		n = nd_ifinfo_indexlim * sizeof(struct nd_ifinfo);
		q = (caddr_t)malloc(n, M_IP6NDP, M_WAITOK);
		bzero(q, n);
		if (nd_ifinfo) {
			bcopy((caddr_t)nd_ifinfo, q, n/2);
			free((caddr_t)nd_ifinfo, M_IP6NDP);
		}
		nd_ifinfo = (struct nd_ifinfo *)q;
	}

#define ND nd_ifinfo[ifp->if_index]

	/*
	 * Don't initialize if called twice.
	 * XXX: to detect this, we should choose a member that is never set
	 * before initialization of the ND structure itself.  We formaly used
	 * the linkmtu member, which was not suitable because it could be 
	 * initialized via "ifconfig mtu".
	 */
	if (ND.basereachable)
		return;

#ifdef DIAGNOSTIC
#if defined(__FreeBSD__) && __FreeBSD__ >= 5
	if (!ifnet_byindex(ifp->if_index))
		panic("nd6_ifattach: ifnet_byindex is NULL");
#else
	if (!ifindex2ifnet[ifp->if_index])
		panic("nd6_ifattach: ifindex2ifnet is NULL");
#endif
#endif
#if defined(__FreeBSD__) && __FreeBSD__ >= 5
	ND.linkmtu = ifnet_byindex(ifp->if_index)->if_mtu;
#else
	ND.linkmtu = ifindex2ifnet[ifp->if_index]->if_mtu;
#endif
	ND.chlim = IPV6_DEFHLIM;
	ND.basereachable = REACHABLE_TIME;
	ND.reachable = ND_COMPUTE_RTIME(ND.basereachable);
	ND.retrans = RETRANS_TIMER;
	ND.receivedra = 0;
	/*
	 * Note that the default value of ip6_accept_rtadv is 0, which means
	 * we won't accept RAs by default even if we set ND6_IFF_ACCEPT_RTADV
	 * here.
	 */
	ND.flags = ND6_IFF_PERFORMNUD | ND6_IFF_ACCEPT_RTADV;
	nd6_setmtu(ifp);
#undef ND
}

/*
 * Reset ND level link MTU. This function is called when the physical MTU
 * changes, which means we might have to adjust the ND level MTU.
 */
void
nd6_setmtu(ifp)
	struct ifnet *ifp;
{
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
	struct nd_ifinfo *ndi = &nd_ifinfo[ifp->if_index];
	u_long oldmaxmtu = ndi->maxmtu;
	u_long oldlinkmtu = ndi->linkmtu;

	switch (ifp->if_type) {
	case IFT_ARCNET:	/* XXX MTU handling needs more work */
		ndi->maxmtu = MIN(60480, ifp->if_mtu);
		break;
	case IFT_ETHER:
		ndi->maxmtu = MIN(ETHERMTU, ifp->if_mtu);
		break;
#if defined(__FreeBSD__) || defined(__bsdi__)
	case IFT_FDDI:
#if 0 // FIXME
#if defined(__bsdi__) && _BSDI_VERSION >= 199802
		ndi->maxmtu = MIN(FDDIMTU, ifp->if_mtu);
#else
		ndi->maxmtu = MIN(FDDIIPMTU, ifp->if_mtu);
#endif
#endif
		break;
#endif
#if !(defined(__bsdi__) && _BSDI_VERSION >= 199802)
	case IFT_ATM:
#if 0 // FIXME
		ndi->maxmtu = MIN(ATMMTU, ifp->if_mtu);
#endif
		break;
#endif
	case IFT_IEEE1394:	/* XXX should be IEEE1394MTU(1500) */
		ndi->maxmtu = MIN(ETHERMTU, ifp->if_mtu);
		break;
#ifdef IFT_IEEE80211
	case IFT_IEEE80211:	/* XXX should be IEEE80211MTU(1500) */
		ndi->maxmtu = MIN(ETHERMTU, ifp->if_mtu);
		break;
#endif
	default:
		ndi->maxmtu = ifp->if_mtu;
		break;
	}

	if (oldmaxmtu != ndi->maxmtu) {
		/*
		 * If the ND level MTU is not set yet, or if the maxmtu
		 * is reset to a smaller value than the ND level MTU,
		 * also reset the ND level MTU.
		 */
		if (ndi->linkmtu == 0 ||
		    ndi->maxmtu < ndi->linkmtu) {
			ndi->linkmtu = ndi->maxmtu;
			/* also adjust in6_maxmtu if necessary. */
			if (oldlinkmtu == 0) {
				/*
				 * XXX: the case analysis is grotty, but
				 * it is not efficient to call in6_setmaxmtu()
				 * here when we are during the initialization
				 * procedure.
				 */
				if (in6_maxmtu < ndi->linkmtu)
					in6_maxmtu = ndi->linkmtu;
			} else
				in6_setmaxmtu();
		}
	}
#undef MIN
}

void
nd6_option_init(opt, icmp6len, ndopts)
	void *opt;
	int icmp6len;
	union nd_opts *ndopts;
{
	bzero(ndopts, sizeof(*ndopts));
	ndopts->nd_opts_search = (struct nd_opt_hdr *)opt;
	ndopts->nd_opts_last
		= (struct nd_opt_hdr *)(((u_char *)opt) + icmp6len);

	if (icmp6len == 0) {
		ndopts->nd_opts_done = 1;
		ndopts->nd_opts_search = NULL;
	}
}

/*
 * Take one ND option.
 */
struct nd_opt_hdr *
nd6_option(ndopts)
	union nd_opts *ndopts;
{
	struct nd_opt_hdr *nd_opt;
	int olen;

	if (!ndopts)
		panic("ndopts == NULL in nd6_option\n");
	if (!ndopts->nd_opts_last)
		panic("uninitialized ndopts in nd6_option\n");
	if (!ndopts->nd_opts_search)
		return NULL;
	if (ndopts->nd_opts_done)
		return NULL;

	nd_opt = ndopts->nd_opts_search;

	/* make sure nd_opt_len is inside the buffer */
	if ((caddr_t)&nd_opt->nd_opt_len >= (caddr_t)ndopts->nd_opts_last) {
		bzero(ndopts, sizeof(*ndopts));
		return NULL;
	}

	olen = nd_opt->nd_opt_len << 3;
	if (olen == 0) {
		/*
		 * Message validation requires that all included
		 * options have a length that is greater than zero.
		 */
		bzero(ndopts, sizeof(*ndopts));
		return NULL;
	}

	ndopts->nd_opts_search = (struct nd_opt_hdr *)((caddr_t)nd_opt + olen);
	if (ndopts->nd_opts_search > ndopts->nd_opts_last) {
		/* option overruns the end of buffer, invalid */
		bzero(ndopts, sizeof(*ndopts));
		return NULL;
	} else if (ndopts->nd_opts_search == ndopts->nd_opts_last) {
		/* reached the end of options chain */
		ndopts->nd_opts_done = 1;
		ndopts->nd_opts_search = NULL;
	}
	return nd_opt;
}

/*
 * Parse multiple ND options.
 * This function is much easier to use, for ND routines that do not need
 * multiple options of the same type.
 */
int
nd6_options(ndopts)
	union nd_opts *ndopts;
{
	struct nd_opt_hdr *nd_opt;
	int i = 0;

	if (!ndopts)
		panic("ndopts == NULL in nd6_options\n");
	if (!ndopts->nd_opts_last)
		panic("uninitialized ndopts in nd6_options\n");
	if (!ndopts->nd_opts_search)
		return 0;

	while (1) {
		nd_opt = nd6_option(ndopts);
		if (!nd_opt && !ndopts->nd_opts_last) {
			/*
			 * Message validation requires that all included
			 * options have a length that is greater than zero.
			 */
			icmp6stat.icp6s_nd_badopt++;
			bzero(ndopts, sizeof(*ndopts));
			return -1;
		}

		if (!nd_opt)
			goto skip1;

		switch (nd_opt->nd_opt_type) {
		case ND_OPT_SOURCE_LINKADDR:
		case ND_OPT_TARGET_LINKADDR:
		case ND_OPT_MTU:
		case ND_OPT_REDIRECTED_HEADER:
		case ND_OPT_ADVINTERVAL:
		case ND_OPT_SOURCE_ADDRLIST:
		case ND_OPT_TARGET_ADDRLIST:
			if (ndopts->nd_opt_array[nd_opt->nd_opt_type]) {
				nd6log((LOG_INFO,
				    "duplicated ND6 option found (type=%d)\n",
				    nd_opt->nd_opt_type));
				/* XXX bark? */
			} else {
				ndopts->nd_opt_array[nd_opt->nd_opt_type]
					= nd_opt;
			}
			break;
		case ND_OPT_PREFIX_INFORMATION:
			if (ndopts->nd_opt_array[nd_opt->nd_opt_type] == 0) {
				ndopts->nd_opt_array[nd_opt->nd_opt_type]
					= nd_opt;
			}
			ndopts->nd_opts_pi_end =
				(struct nd_opt_prefix_info *)nd_opt;
			break;
		case ND_OPT_HOMEAGENT_INFO:
			break;
		default:
			/*
			 * Unknown options must be silently ignored,
			 * to accomodate future extension to the protocol.
			 */
			nd6log((LOG_DEBUG,
			    "nd6_options: unsupported option %d - "
			    "option ignored\n", nd_opt->nd_opt_type));
		}

skip1:
		i++;
		if (i > nd6_maxndopt) {
			icmp6stat.icp6s_nd_toomanyopt++;
			nd6log((LOG_INFO, "too many loop in nd opt\n"));
			break;
		}

		if (ndopts->nd_opts_done)
			break;
	}

	return 0;
}

/*
 * ND6 timer routine to expire default route list and prefix list
 */
void
nd6_timer(ignored_arg)
	void	*ignored_arg;
{
	int s;
	struct llinfo_nd6 *ln;
	struct nd_defrouter *dr;
	struct nd_prefix *pr;
#if !(defined(__FreeBSD__) && __FreeBSD__ >= 3)
	long time_second = time.tv_sec;
#endif
	struct ifnet *ifp;
	struct in6_ifaddr *ia6, *nia6;
	struct in6_addrlifetime *lt6;
	
#ifdef __NetBSD__
	s = splsoftnet();
#else
	s = splnet();
#endif
#if defined(__NetBSD__) || (defined(__FreeBSD__) && __FreeBSD__ >= 3)
	callout_reset(&nd6_timer_ch, nd6_prune * hz,
	    nd6_timer, NULL);
#elif defined(__OpenBSD__)
	timeout_set(&nd6_timer_ch, nd6_timer, NULL);
	timeout_add(&nd6_timer_ch, nd6_prune * hz);
#else
	timeout(nd6_timer, (caddr_t)0, nd6_prune * hz);
#endif

	ln = llinfo_nd6.ln_next;
	while (ln && ln != &llinfo_nd6) {
		struct rtentry *rt;
		struct sockaddr_in6 *dst;
		struct llinfo_nd6 *next = ln->ln_next;
		/* XXX: used for the DELAY case only: */
		struct nd_ifinfo *ndi = NULL;

		if ((rt = ln->ln_rt) == NULL) {
			ln = next;
			continue;
		}
		if ((ifp = rt->rt_ifp) == NULL) {
			ln = next;
			continue;
		}
		ndi = &nd_ifinfo[ifp->if_index];
		dst = (struct sockaddr_in6 *)rt_key(rt);

		if (ln->ln_expire > time_second) {
			ln = next;
			continue;
		}

		/* sanity check */
		if (!rt)
			panic("rt=0 in nd6_timer(ln=%p)\n", ln);
		if (rt->rt_llinfo && (struct llinfo_nd6 *)rt->rt_llinfo != ln)
			panic("rt_llinfo(%p) is not equal to ln(%p)\n",
			      rt->rt_llinfo, ln);
		if (!dst)
			panic("dst=0 in nd6_timer(ln=%p)\n", ln);

		switch (ln->ln_state) {
		case ND6_LLINFO_INCOMPLETE:
			if (ln->ln_asked < nd6_mmaxtries) {
				ln->ln_asked++;
				ln->ln_expire = time_second +
					nd_ifinfo[ifp->if_index].retrans / 1000;
				nd6_ns_output(ifp, NULL, &dst->sin6_addr,
					ln, 0);
			} else {
				struct mbuf *m = ln->ln_hold;
				if (m) {
					struct ip6_hdr *ip6_in;
					struct sockaddr_in6 sin6_in;
					int64_t szoneid, dzoneid;

					/*
					 * Fake rcvif to make the ICMP error
					 * more helpful in diagnosing for the
					 * receiver.
					 * XXX: should we consider
					 * older rcvif?
					 */
					m->m_pkthdr.rcvif = rt->rt_ifp;

					/*
					 * XXX: for scoped addresses, we should
					 * disambiguate the zone.  We should
					 * perhaps hang sockaddr_in6 as aux
					 * data in the mbuf.
					 */
					ip6_in = mtod(m, struct ip6_hdr *);
					szoneid = in6_addr2zoneid(rt->rt_ifp,
								  &ip6_in->ip6_src);
					dzoneid = in6_addr2zoneid(rt->rt_ifp,
								  &ip6_in->ip6_dst);
					if (szoneid < 0 || dzoneid < 0) {
						/* impossible... */
						m_freem(m);
					} else {
						bzero(&sin6_in,
						      sizeof(sin6_in));
						sin6_in.sin6_addr = ip6_in->ip6_src;
						sin6_in.sin6_scope_id = szoneid;
						in6_embedscope(&ip6_in->ip6_src,
							       &sin6_in);
						bzero(&sin6_in,
						      sizeof(sin6_in));
						sin6_in.sin6_addr = ip6_in->ip6_dst;
						sin6_in.sin6_scope_id = dzoneid;
						in6_embedscope(&ip6_in->ip6_dst,
							       &sin6_in);
						icmp6_error(m,
							    ICMP6_DST_UNREACH,
							    ICMP6_DST_UNREACH_ADDR, 0);
					}
					ln->ln_hold = NULL;
				}
				next = nd6_free(rt, 0);
			}
			break;
		case ND6_LLINFO_REACHABLE:
			if (ln->ln_expire) {
				ln->ln_state = ND6_LLINFO_STALE;
				ln->ln_expire = time_second + nd6_gctimer;
			}
			break;

		case ND6_LLINFO_STALE:
			/* Garbage Collection(RFC 2461 5.3) */
			if (ln->ln_expire)
				next = nd6_free(rt, 1);
			break;

		case ND6_LLINFO_DELAY:
			if (ndi && (ndi->flags & ND6_IFF_PERFORMNUD) != 0) {
				/* We need NUD */
				ln->ln_asked = 1;
				ln->ln_state = ND6_LLINFO_PROBE;
				ln->ln_expire = time_second +
					ndi->retrans / 1000;
				nd6_ns_output(ifp, &dst->sin6_addr,
					      &dst->sin6_addr,
					      ln, 0);
			} else {
				ln->ln_state = ND6_LLINFO_STALE; /* XXX */
				ln->ln_expire = time_second + nd6_gctimer;
			}
			break;
		case ND6_LLINFO_PROBE:
			if (ln->ln_asked < nd6_umaxtries) {
				ln->ln_asked++;
				ln->ln_expire = time_second +
					nd_ifinfo[ifp->if_index].retrans / 1000;
				nd6_ns_output(ifp, &dst->sin6_addr,
					       &dst->sin6_addr, ln, 0);
			} else {
				next = nd6_free(rt, 0);
			}
			break;
		}
		ln = next;
	}
	
	/* expire default router list */
	dr = TAILQ_FIRST(&nd_defrouter);
	while (dr) {
		if (dr->expire && dr->expire < time_second) {
			struct nd_defrouter *t;
			t = TAILQ_NEXT(dr, dr_entry);
			defrtrlist_del(dr);
			dr = t;
		} else {
			dr = TAILQ_NEXT(dr, dr_entry);
		}
	}

	/*
	 * expire interface addresses.
	 * in the past the loop was inside prefix expiry processing.
	 * However, from a stricter speci-confrmance standpoint, we should
	 * rather separate address lifetimes and prefix lifetimes.
	 */
  addrloop:
	for (ia6 = in6_ifaddr; ia6; ia6 = nia6) {
		nia6 = ia6->ia_next;
		/* check address lifetime */
		lt6 = &ia6->ia6_lifetime;
		if (IFA6_IS_INVALID(ia6)) {
			int regen = 0;

			/*
			 * If the expiring address is temporary, try
			 * regenerating a new one.  This would be useful when
			 * we suspended a laptop PC, then turned it on after a
			 * period that could invalidate all temporary
			 * addresses.  Although we may have to restart the
			 * loop (see below), it must be after purging the
			 * address.  Otherwise, we'd see an infinite loop of
			 * regeneration. 
			 */
			if (ip6_use_tempaddr &&
			    (ia6->ia6_flags & IN6_IFF_TEMPORARY) != 0) {
				if (regen_tmpaddr(ia6) == 0)
					regen = 1;
			}

			in6_purgeaddr(&ia6->ia_ifa);

			if (regen)
				goto addrloop; /* XXX: see below */
		}
		if (IFA6_IS_DEPRECATED(ia6)) {
			int oldflags = ia6->ia6_flags;

			ia6->ia6_flags |= IN6_IFF_DEPRECATED;

			/*
			 * If a temporary address has just become deprecated,
			 * regenerate a new one if possible.
			 */
			if (ip6_use_tempaddr &&
			    (ia6->ia6_flags & IN6_IFF_TEMPORARY) != 0 &&
			    (oldflags & IN6_IFF_DEPRECATED) == 0) {

				if (regen_tmpaddr(ia6) == 0) {
					/*
					 * A new temporary address is
					 * generated.
					 * XXX: this means the address chain
					 * has changed while we are still in
					 * the loop.  Although the change
					 * would not cause disaster (because
					 * it's not a deletion, but an
					 * addition,) we'd rather restart the
					 * loop just for safety.  Or does this 
					 * significantly reduce performance??
					 */
					goto addrloop;
				}
			}
		} else {
			/*
			 * A new RA might have made a deprecated address
			 * preferred.
			 */
			ia6->ia6_flags &= ~IN6_IFF_DEPRECATED;
		}
	}

	/* expire prefix list */
	pr = nd_prefix.lh_first;
	while (pr) {
		/*
		 * check prefix lifetime.
		 * since pltime is just for autoconf, pltime processing for
		 * prefix is not necessary.
		 */
		if (pr->ndpr_vltime != ND6_INFINITE_LIFETIME &&
		    time_second - pr->ndpr_lastupdate > pr->ndpr_vltime) {
			struct nd_prefix *t;
			t = pr->ndpr_next;

			/*
			 * address expiration and prefix expiration are
			 * separate.  NEVER perform in6_purgeaddr here.
			 */

			prelist_remove(pr);
			pr = t;
		} else
			pr = pr->ndpr_next;
	}
	splx(s);
}

static int
regen_tmpaddr(ia6)
	struct in6_ifaddr *ia6; /* deprecated/invalidated temporary address */
{
	struct ifaddr *ifa;
	struct ifnet *ifp;
	struct in6_ifaddr *public_ifa6 = NULL;

	ifp = ia6->ia_ifa.ifa_ifp;
#if defined(__bsdi__) || (defined(__FreeBSD__) && __FreeBSD__ < 3)
	for (ifa = ifp->if_addrlist; ifa; ifa = ifa->ifa_next)
#else
	for (ifa = ifp->if_addrlist.tqh_first; ifa;
	     ifa = ifa->ifa_list.tqe_next)
#endif
	{
		struct in6_ifaddr *it6;

		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;

		it6 = (struct in6_ifaddr *)ifa;

		/* ignore no autoconf addresses. */
		if ((it6->ia6_flags & IN6_IFF_AUTOCONF) == 0)
			continue;

		/* ignore autoconf addresses with different prefixes. */
		if (it6->ia6_ndpr == NULL || it6->ia6_ndpr != ia6->ia6_ndpr)
			continue;

		/*
		 * Now we are looking at an autoconf address with the same
		 * prefix as ours.  If the address is temporary and is still
		 * preferred, do not create another one.  It would be rare, but
		 * could happen, for example, when we resume a laptop PC after
		 * a long period.
		 */
		if ((it6->ia6_flags & IN6_IFF_TEMPORARY) != 0 &&
		    !IFA6_IS_DEPRECATED(it6)) {
			public_ifa6 = NULL;
			break;
		}

		/*
		 * This is a public autoconf address that has the same prefix
		 * as ours.  If it is preferred, keep it.  We can't break the
		 * loop here, because there may be a still-preferred temporary
		 * address with the prefix.
		 */
		if (!IFA6_IS_DEPRECATED(it6))
		    public_ifa6 = it6;
	}

	if (public_ifa6 != NULL) {
		int e;

		if ((e = in6_tmpifadd(public_ifa6, 0)) != 0) {
			log(LOG_NOTICE, "regen_tmpaddr: failed to create a new"
			    " tmp addr,errno=%d\n", e);
			return(-1);
		}
		return(0);
	}

	return(-1);
}

/*
 * Nuke neighbor cache/prefix/default router management table, right before
 * ifp goes away.
 */
void
nd6_purge(ifp)
	struct ifnet *ifp;
{
	struct llinfo_nd6 *ln, *nln;
	struct nd_defrouter *dr, *ndr;
	struct nd_prefix *pr, *npr;

	/*
	 * Nuke default router list entries toward ifp.
	 * We defer removal of default router list entries that is installed
	 * in the routing table, in order to keep additional side effects as
	 * small as possible.
	 */
	for (dr = TAILQ_FIRST(&nd_defrouter); dr; dr = ndr) {
		ndr = TAILQ_NEXT(dr, dr_entry);
		if (dr->installed)
			continue;

		if (dr->ifp == ifp)
			defrtrlist_del(dr);
	}
	for (dr = TAILQ_FIRST(&nd_defrouter); dr; dr = ndr) {
		ndr = TAILQ_NEXT(dr, dr_entry);
		if (!dr->installed)
			continue;

		if (dr->ifp == ifp)
			defrtrlist_del(dr);
	}

	/* Nuke prefix list entries toward ifp */
	for (pr = nd_prefix.lh_first; pr; pr = npr) {
		npr = pr->ndpr_next;
		if (pr->ndpr_ifp == ifp) {
			/*
			 * Previously, pr->ndpr_addr is removed as well,
			 * but I strongly believe we don't have to do it.
			 * nd6_purge() is only called from in6_ifdetach(),
			 * which removes all the associated interface addresses
			 * by itself.
			 * (jinmei@kame.net 20010129)
			 */
			prelist_remove(pr);
		}
	}

	/* cancel default outgoing interface setting */
	if (nd6_defifindex == ifp->if_index)
		nd6_setdefaultiface(0);

	if (!ip6_forwarding && ip6_accept_rtadv) { /* XXX: too restrictive? */
		/* refresh default router list */
		defrouter_select();
	}

	/*
	 * Nuke neighbor cache entries for the ifp.
	 * Note that rt->rt_ifp may not be the same as ifp,
	 * due to KAME goto ours hack.  See RTM_RESOLVE case in
	 * nd6_rtrequest(), and ip6_input().
	 */
	ln = llinfo_nd6.ln_next;
	while (ln && ln != &llinfo_nd6) {
		struct rtentry *rt;
		struct sockaddr_dl *sdl;

		nln = ln->ln_next;
		rt = ln->ln_rt;
		if (rt && rt->rt_gateway &&
		    rt->rt_gateway->sa_family == AF_LINK) {
			sdl = (struct sockaddr_dl *)rt->rt_gateway;
			if (sdl->sdl_index == ifp->if_index)
				nln = nd6_free(rt, 0);
		}
		ln = nln;
	}
}

struct rtentry *
nd6_lookup(addr6, create, ifp)
	struct in6_addr *addr6;
	int create;
	struct ifnet *ifp;
{
	struct rtentry *rt;
	struct sockaddr_in6 sin6;
#ifdef SCOPEDROUTING
	int64_t zoneid;
#endif

	bzero(&sin6, sizeof(sin6));
	sin6.sin6_len = sizeof(struct sockaddr_in6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_addr = *addr6;
#ifdef SCOPEDROUTING
	if ((zoneid = in6_addr2zoneid(ifp, addr6)) < 0)
		return(NULL);
	sin6.sin6_scope_id = zoneid;
#endif
	rt = rtalloc1((struct sockaddr *)&sin6, create
#ifdef __FreeBSD__
		      , 0UL
#endif /* __FreeBSD__ */
		      );
	if (rt && (rt->rt_flags & RTF_LLINFO) == 0) {
		/*
		 * This is the case for the default route.
		 * If we want to create a neighbor cache for the address, we
		 * should free the route for the destination and allocate an
		 * interface route.
		 */
		if (create) {
			RTFREE(rt);
			rt = 0;
		}
	}
	if (!rt) {
		if (create && ifp) {
			int e;

			/*
			 * If no route is available and create is set,
			 * we allocate a host route for the destination
			 * and treat it like an interface route.
			 * This hack is necessary for a neighbor which can't
			 * be covered by our own prefix.
			 */
			struct ifaddr *ifa =
				ifaof_ifpforaddr((struct sockaddr *)&sin6, ifp);
			if (ifa == NULL)
				return(NULL);

			/*
			 * Create a new route.  RTF_LLINFO is necessary
			 * to create a Neighbor Cache entry for the
			 * destination in nd6_rtrequest which will be
			 * called in rtrequest via ifa->ifa_rtrequest.
			 */
			if ((e = rtrequest(RTM_ADD, (struct sockaddr *)&sin6,
					   ifa->ifa_addr,
					   (struct sockaddr *)&all1_sa,
					   (ifa->ifa_flags |
					    RTF_HOST | RTF_LLINFO) &
					   ~RTF_CLONING,
					   &rt)) != 0) {
#if 0
				log(LOG_ERR,
				    "nd6_lookup: failed to add route for a "
				    "neighbor(%s), errno=%d\n",
				    ip6_sprintf(addr6), e);
#endif
				return(NULL);
			}
			if (rt == NULL)
				return(NULL);
			if (rt->rt_llinfo) {
				struct llinfo_nd6 *ln =
					(struct llinfo_nd6 *)rt->rt_llinfo;
				ln->ln_state = ND6_LLINFO_NOSTATE;
			}
		} else
			return(NULL);
	}
	rt->rt_refcnt--;
	/*
	 * Validation for the entry.
	 * XXX: we can't use rt->rt_ifp to check for the interface, since
	 *      it might be the loopback interface if the entry is for our
	 *      own address on a non-loopback interface. Instead, we should
	 *      use rt->rt_ifa->ifa_ifp, which would specify the REAL interface.
	 */
	if ((rt->rt_flags & RTF_GATEWAY) || (rt->rt_flags & RTF_LLINFO) == 0 ||
	    rt->rt_gateway->sa_family != AF_LINK ||
	    (ifp && rt->rt_ifa->ifa_ifp != ifp)) {
		if (create) {
			log(LOG_DEBUG, "nd6_lookup: failed to lookup %s (if = %s)\n",
			    ip6_sprintf(addr6), ifp ? if_name(ifp) : "unspec");
		}
		return(0);
	}
	return(rt);
}

/*
 * Detect if a given IPv6 address identifies a neighbor on a given link.
 * XXX: should take care of the destination of a p2p link?
 */
int
nd6_is_addr_neighbor(addr, ifp)
	struct sockaddr_in6 *addr;
	struct ifnet *ifp;
{
	struct nd_prefix *pr;
	struct rtentry *rt;
	int i;

#define IFADDR6(a) ((((struct in6_ifaddr *)(a))->ia_addr).sin6_addr)
#define IFMASK6(a) ((((struct in6_ifaddr *)(a))->ia_prefixmask).sin6_addr)

	/*
	 * A link-local address is always a neighbor.
	 * XXX: we should use the sin6_scope_id field rather than the embedded
	 * interface index.
	 * XXX: a link does not necessarily specify a single interface.
	 */
	//if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr) &&
	//    ntohs(*(u_int16_t *)&addr->sin6_addr.s6_addr[2]) == ifp->if_index)
	//	return(1);

    // SD - Don't verify the looked up route matches the interface number of the socket. 
    // This will cause the packet to be dropped (NOROUTE).  We actually want this to pass
    // when we have 2 interfaces on the same link. 
    if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr) )
        return(1);


	/*
	 * If the address matches one of our on-link prefixes, it should be a
	 * neighbor.
	 */
	for (pr = nd_prefix.lh_first; pr; pr = pr->ndpr_next) {

		if (pr->ndpr_ifp != ifp)
			continue;

		if (!(pr->ndpr_stateflags & NDPRF_ONLINK))
			continue;

		for (i = 0; i < 4; i++) {
			if ((pr->ndpr_mask.s6_addr32[i] &
			     addr->sin6_addr.s6_addr32[i]) !=
			    pr->ndpr_prefix.sin6_addr.s6_addr32[i])
				break;
		}
		if (i == 4)	/* full match */
			return(1);
	}

	/*
	 * If the default router list is empty, all addresses are regarded
	 * as on-link, and thus, as a neighbor.
	 * XXX: we restrict the condition to hosts, because routers usually do
	 * not have the "default router list".
	 */
	if (!ip6_forwarding && TAILQ_FIRST(&nd_defrouter) == NULL &&
	    nd6_defifindex == ifp->if_index) {
		return(1);
	}

	/*
	 * Even if the address matches none of our addresses, it might be
	 * in the neighbor cache.
	 */
	if ((rt = nd6_lookup(&addr->sin6_addr, 0, ifp)) != NULL &&
	    rt->rt_llinfo != NULL)
		return(1);

	return(0);
#undef IFADDR6
#undef IFMASK6
}

/*
 * Free an nd6 llinfo entry.
 * Since the function would cause significant changes in the kernel, DO NOT
 * make it global, unless you have a strong reason for the change, and are sure
 * that the change is safe.
 */
static struct llinfo_nd6 *
nd6_free(rt, gc)
	struct rtentry *rt;
	int gc;
{
	struct llinfo_nd6 *ln = (struct llinfo_nd6 *)rt->rt_llinfo, *next;
	struct in6_addr in6 = ((struct sockaddr_in6 *)rt_key(rt))->sin6_addr;
	struct nd_defrouter *dr;

	/*
	 * we used to have pfctlinput(PRC_HOSTDEAD) here.
	 * even though it is not harmful, it was not really necessary.
	 */

	if (!ip6_forwarding && ip6_accept_rtadv) { /* XXX: too restrictive? */
		int s;
#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		dr = defrouter_lookup(&((struct sockaddr_in6 *)rt_key(rt))->sin6_addr,
				      rt->rt_ifp);

		if (dr != NULL && dr->expire &&
		    ln->ln_state == ND6_LLINFO_STALE && gc) {
			/*
			 * If the reason for the deletion is just garbage
			 * collection, and the neighbor is an active default
			 * router, do not delete it.  Instead, reset the GC
			 * timer using the router's lifetime.
			 * Simply deleting the entry would affect default
			 * router selection, which is not necessarily a good
			 * thing, especially when we're using router preference
			 * values.
			 * XXX: the check for ln_state would be redundant,
			 *      but we intentionally keep it just in case.
			 */
			ln->ln_expire = dr->expire;
			splx(s);
			return(ln->ln_next);
		}

		if (ln->ln_router || dr) {
			/*
			 * rt6_flush must be called whether or not the neighbor
			 * is in the Default Router List.
			 * See a corresponding comment in nd6_na_input().
			 */
			rt6_flush(&in6, rt->rt_ifp);
		}

		if (dr) {
			/*
			 * Unreachablity of a router might affect the default
			 * router selection and on-link detection of advertised
			 * prefixes.
			 */

			/*
			 * Temporarily fake the state to choose a new default
			 * router and to perform on-link determination of
			 * prefixes correctly.
			 * Below the state will be set correctly,
			 * or the entry itself will be deleted.
			 */
			ln->ln_state = ND6_LLINFO_INCOMPLETE;

			/*
			 * Since defrouter_select() does not affect the
			 * on-link determination and MIP6 needs the check
			 * before the default router selection, we perform
			 * the check now.
			 */
			pfxlist_onlink_check();

			/*
			 * refresh default router list
			 */
			defrouter_select();
		}
		splx(s);
	}

	/*
	 * Before deleting the entry, remember the next entry as the
	 * return value.  We need this because pfxlist_onlink_check() above
	 * might have freed other entries (particularly the old next entry) as
	 * a side effect (XXX).
	 */
	next = ln->ln_next;

	/*
	 * Detach the route from the routing tree and the list of neighbor
	 * caches, and disable the route entry not to be used in already
	 * cached routes.
	 */
	rtrequest(RTM_DELETE, rt_key(rt), (struct sockaddr *)0,
		  rt_mask(rt), 0, (struct rtentry **)0);

	return(next);
}

/*
 * Upper-layer reachability hint for Neighbor Unreachability Detection.
 *
 * XXX cost-effective metods?
 */
void
nd6_nud_hint(rt, dst6, force)
	struct rtentry *rt;
	struct in6_addr *dst6;
	int force;
{
	struct llinfo_nd6 *ln;
#if !(defined(__FreeBSD__) && __FreeBSD__ >= 3)
	long time_second = time.tv_sec;
#endif

	/*
	 * If the caller specified "rt", use that.  Otherwise, resolve the
	 * routing table by supplied "dst6".
	 */
	if (!rt) {
		if (!dst6)
			return;
		if (!(rt = nd6_lookup(dst6, 0, NULL)))
			return;
	}

	if ((rt->rt_flags & RTF_GATEWAY) != 0 ||
	    (rt->rt_flags & RTF_LLINFO) == 0 ||
	    !rt->rt_llinfo || !rt->rt_gateway ||
	    rt->rt_gateway->sa_family != AF_LINK) {
		/* This is not a host route. */
		return;
	}

	ln = (struct llinfo_nd6 *)rt->rt_llinfo;
	if (ln->ln_state < ND6_LLINFO_REACHABLE)
		return;

	/*
	 * if we get upper-layer reachability confirmation many times,
	 * it is possible we have false information.
	 */
	if (!force) {
		ln->ln_byhint++;
		if (ln->ln_byhint > nd6_maxnudhint)
			return;
	}

	ln->ln_state = ND6_LLINFO_REACHABLE;
	if (ln->ln_expire)
		ln->ln_expire = time_second +
			nd_ifinfo[rt->rt_ifp->if_index].reachable;
}

void
#if (defined(__bsdi__) && _BSDI_VERSION >= 199802) || defined(__NetBSD__) || defined(__OpenBSD__)
nd6_rtrequest(req, rt, info)
	int	req;
	struct rtentry *rt;
	struct rt_addrinfo *info; /* xxx unused */
#else
nd6_rtrequest(req, rt, sa)
	int	req;
	struct rtentry *rt;
	struct sockaddr *sa; /* xxx unused */
#endif
{
	struct sockaddr *gate = rt->rt_gateway;
	struct llinfo_nd6 *ln = (struct llinfo_nd6 *)rt->rt_llinfo;
	static struct sockaddr_dl null_sdl = {sizeof(null_sdl), AF_LINK};
	struct ifnet *ifp = rt->rt_ifp;
	struct ifaddr *ifa;
#if !(defined(__FreeBSD__) && __FreeBSD__ >= 3)
	long time_second = time.tv_sec;
#endif

	if ((rt->rt_flags & RTF_GATEWAY))
		return;

	if (nd6_need_cache(ifp) == 0 && (rt->rt_flags & RTF_HOST) == 0) {
		/*
		 * This is probably an interface direct route for a link
		 * which does not need neighbor caches (e.g. fe80::%lo0/64).
		 * We do not need special treatment below for such a route.
		 * Moreover, the RTF_LLINFO flag which would be set below
		 * would annoy the ndp(8) command.
		 */
		return;
	}

	if (req == RTM_RESOLVE &&
	    !nd6_is_addr_neighbor((struct sockaddr_in6 *)rt_key(rt), ifp)) {
		/*
		 * FreeBSD and BSD/OS often make a cloned host route based
		 * on a less-specific route (e.g. the default route).
		 * If the less specific route does not have a "gateway"
		 * (this is the case when the route just goes to a p2p
		 * interface), we'll mistakenly make a neighbor cache for
		 * the host route, and will see strange neighbor solicitation
		 * for the corresponding destination.  In order to avoid the
		 * confusion, we check if the destination of the route is
		 * a neighbor in terms of neighbor discovery, and stop the
		 * process if not.  Additionally, we remove the LLINFO flag
		 * so that ndp(8) will not try to get the neighbor information
		 * of the destination.
		 */
		rt->rt_flags &= ~RTF_LLINFO;
		return;
	}

	switch (req) {
	case RTM_ADD:
		/*
		 * There is no backward compatibility :)
		 *
		 * if ((rt->rt_flags & RTF_HOST) == 0 &&
		 *     SIN(rt_mask(rt))->sin_addr.s_addr != 0xffffffff)
		 *	   rt->rt_flags |= RTF_CLONING;
		 */
		if (rt->rt_flags & (RTF_CLONING | RTF_LLINFO)) {
			/*
			 * Case 1: This route should come from
			 * a route to interface.  RTF_LLINFO flag is set
			 * for a host route whose destination should be
			 * treated as on-link.
			 */
			rt_setgate(rt, rt_key(rt),
				   (struct sockaddr *)&null_sdl);
			gate = rt->rt_gateway;
			SDL(gate)->sdl_type = ifp->if_type;
			SDL(gate)->sdl_index = ifp->if_index;
			if (ln)
				ln->ln_expire = time_second;
#if 1
			if (ln && ln->ln_expire == 0) {
				/* kludge for desktops */
#if 0
				printf("nd6_rtequest: time.tv_sec is zero; "
				       "treat it as 1\n");
#endif
				ln->ln_expire = 1;
			}
#endif
			if ((rt->rt_flags & RTF_CLONING))
				break;
		}
		/*
		 * In IPv4 code, we try to annonuce new RTF_ANNOUNCE entry here.
		 * We don't do that here since llinfo is not ready yet.
		 *
		 * There are also couple of other things to be discussed:
		 * - unsolicited NA code needs improvement beforehand
		 * - RFC2461 says we MAY send multicast unsolicited NA
		 *   (7.2.6 paragraph 4), however, it also says that we
		 *   SHOULD provide a mechanism to prevent multicast NA storm.
		 *   we don't have anything like it right now.
		 *   note that the mechanism needs a mutual agreement
		 *   between proxies, which means that we need to implement
		 *   a new protocol, or a new kludge.
		 * - from RFC2461 6.2.4, host MUST NOT send an unsolicited NA.
		 *   we need to check ip6forwarding before sending it.
		 *   (or should we allow proxy ND configuration only for
		 *   routers?  there's no mention about proxy ND from hosts)
		 */
#if 0
		/* XXX it does not work */
		if (rt->rt_flags & RTF_ANNOUNCE)
			nd6_na_output(ifp,
			      &SIN6(rt_key(rt))->sin6_addr,
			      &SIN6(rt_key(rt))->sin6_addr,
			      ip6_forwarding ? ND_NA_FLAG_ROUTER : 0,
			      1, NULL);
#endif
		/* FALLTHROUGH */
	case RTM_RESOLVE:
		if ((ifp->if_flags & (IFF_POINTOPOINT | IFF_LOOPBACK)) == 0) {
			/*
			 * Address resolution isn't necessary for a point to
			 * point link, so we can skip this test for a p2p link.
			 */
			if (gate->sa_family != AF_LINK ||
			    gate->sa_len < sizeof(null_sdl)) {
				log(LOG_DEBUG,
				    "nd6_rtrequest: bad gateway value: %s\n",
				    if_name(ifp));
				break;
			}
			SDL(gate)->sdl_type = ifp->if_type;
			SDL(gate)->sdl_index = ifp->if_index;
		}
		if (ln != NULL)
			break;	/* This happens on a route change */
		/*
		 * Case 2: This route may come from cloning, or a manual route
		 * add with a LL address.
		 */
		R_Malloc(ln, struct llinfo_nd6 *, sizeof(*ln));
		rt->rt_llinfo = (caddr_t)ln;
		if (!ln) {
			log(LOG_DEBUG, "nd6_rtrequest: malloc failed\n");
			break;
		}
		nd6_inuse++;
		nd6_allocated++;
		Bzero(ln, sizeof(*ln));
		ln->ln_rt = rt;
		/* this is required for "ndp" command. - shin */
		if (req == RTM_ADD) {
		        /*
			 * gate should have some valid AF_LINK entry,
			 * and ln->ln_expire should have some lifetime
			 * which is specified by ndp command.
			 */
			ln->ln_state = ND6_LLINFO_REACHABLE;
			ln->ln_byhint = 0;
		} else {
		        /*
			 * When req == RTM_RESOLVE, rt is created and
			 * initialized in rtrequest(), so rt_expire is 0.
			 */
			ln->ln_state = ND6_LLINFO_NOSTATE;
			ln->ln_expire = time_second;
		}
		rt->rt_flags |= RTF_LLINFO;
		ln->ln_next = llinfo_nd6.ln_next;
		llinfo_nd6.ln_next = ln;
		ln->ln_prev = &llinfo_nd6;
		ln->ln_next->ln_prev = ln;

		/*
		 * check if rt_key(rt) is one of my address assigned
		 * to the interface.
		 */
		ifa = (struct ifaddr *)in6ifa_ifpwithaddr(rt->rt_ifp,
					  &SIN6(rt_key(rt))->sin6_addr);
		if (ifa) {
			caddr_t macp = nd6_ifptomac(ifp);
			ln->ln_expire = 0;
			ln->ln_state = ND6_LLINFO_REACHABLE;
			ln->ln_byhint = 0;
			if (macp) {
				Bcopy(macp, LLADDR(SDL(gate)), ifp->if_addrlen);
				SDL(gate)->sdl_alen = ifp->if_addrlen;
			}
			if (nd6_useloopback) {
#ifdef __bsdi__
#if _BSDI_VERSION >= 199802
				extern struct ifnet *loifp;
				rt->rt_ifp = loifp;	/* XXX */
#else
				extern struct ifnet loif;
				rt->rt_ifp = &loif;	/* XXX */
#endif
#elif defined(__OpenBSD__)
				rt->rt_ifp = lo0ifp;	/* XXX */
#else
				rt->rt_ifp = &loif[0];	/* XXX */
#endif
				/*
				 * Make sure rt_ifa be equal to the ifaddr
				 * corresponding to the address.
				 * We need this because when we refer
				 * rt_ifa->ia6_flags in ip6_input, we assume
				 * that the rt_ifa points to the address instead
				 * of the loopback address.
				 */
				if (ifa != rt->rt_ifa) {
					IFAFREE(rt->rt_ifa);
					IFAREF(ifa);
					rt->rt_ifa = ifa;
				}
			}
		} else if (rt->rt_flags & RTF_ANNOUNCE) {
			ln->ln_expire = 0;
			ln->ln_state = ND6_LLINFO_REACHABLE;
			ln->ln_byhint = 0;

			/* join solicited node multicast for proxy ND */
			if (ifp->if_flags & IFF_MULTICAST) {
				struct in6_addr llsol;
				int error;

				llsol = SIN6(rt_key(rt))->sin6_addr;
				llsol.s6_addr16[0] = htons(0xff02);
				llsol.s6_addr16[1] = htons(ifp->if_index);
				llsol.s6_addr32[1] = 0;
				llsol.s6_addr32[2] = htonl(1);
				llsol.s6_addr8[12] = 0xff;

				if (!in6_addmulti(&llsol, ifp, &error)) {
					nd6log((LOG_ERR, "%s: failed to join "
					    "%s (errno=%d)\n", if_name(ifp),
					    ip6_sprintf(&llsol), error));
				}
			}
		}
		break;

	case RTM_DELETE:
		if (!ln)
			break;
		/* leave from solicited node multicast for proxy ND */
		if ((rt->rt_flags & RTF_ANNOUNCE) != 0 &&
		    (ifp->if_flags & IFF_MULTICAST) != 0) {
			struct in6_addr llsol;
			struct in6_multi *in6m;

			llsol = SIN6(rt_key(rt))->sin6_addr;
			llsol.s6_addr16[0] = htons(0xff02);
			llsol.s6_addr16[1] = htons(ifp->if_index);
			llsol.s6_addr32[1] = 0;
			llsol.s6_addr32[2] = htonl(1);
			llsol.s6_addr8[12] = 0xff;

			IN6_LOOKUP_MULTI(llsol, ifp, in6m);
			if (in6m)
				in6_delmulti(in6m);
		}
		nd6_inuse--;
		ln->ln_next->ln_prev = ln->ln_prev;
		ln->ln_prev->ln_next = ln->ln_next;
		ln->ln_prev = NULL;
		rt->rt_llinfo = 0;
		rt->rt_flags &= ~RTF_LLINFO;
		if (ln->ln_hold)
			m_freem(ln->ln_hold);
		Free((caddr_t)ln);
	}
}

int
nd6_ioctl(cmd, data, ifp)
	u_long cmd;
	caddr_t	data;
	struct ifnet *ifp;
{
	struct in6_drlist *drl = (struct in6_drlist *)data;
	struct in6_oprlist *oprl = (struct in6_oprlist *)data;
	struct in6_ndireq *ndi = (struct in6_ndireq *)data;
	struct in6_nbrinfo *nbi = (struct in6_nbrinfo *)data;
	struct in6_ndifreq *ndif = (struct in6_ndifreq *)data;
	struct nd_defrouter *dr;
	struct nd_prefix *pr;
	struct rtentry *rt;
	int i = 0, error = 0;
	int s;

	switch (cmd) {
	case SIOCGDRLST_IN6:
		/*
		 * obsolete API, use sysctl under net.inet6.icmp6
		 */
		bzero(drl, sizeof(*drl));
#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		dr = TAILQ_FIRST(&nd_defrouter);
		while (dr && i < DRLSTSIZ) {
			drl->defrouter[i].rtaddr = dr->rtaddr;
			if (IN6_IS_ADDR_LINKLOCAL(&drl->defrouter[i].rtaddr)) {
				/* XXX: need to this hack for KAME stack */
				drl->defrouter[i].rtaddr.s6_addr16[1] = 0;
			} else
				log(LOG_ERR,
				    "default router list contains a "
				    "non-linklocal address(%s)\n",
				    ip6_sprintf(&drl->defrouter[i].rtaddr));

			drl->defrouter[i].flags = dr->flags;
			drl->defrouter[i].rtlifetime = dr->rtlifetime;
			drl->defrouter[i].expire = dr->expire;
			drl->defrouter[i].if_index = dr->ifp->if_index;
			i++;
			dr = TAILQ_NEXT(dr, dr_entry);
		}
		splx(s);
		break;
	case SIOCGPRLST_IN6:
		/*
		 * obsolete API, use sysctl under net.inet6.icmp6
		 *
		 * XXX the structure in6_prlist was changed in backward-
		 * incompatible manner.  in6_oprlist is used for SIOCGPRLST_IN6,
		 * in6_prlist is used for nd6_sysctl() - fill_prlist().
		 */
		/*
		 * XXX meaning of fields, especialy "raflags", is very
		 * differnet between RA prefix list and RR/static prefix list.
		 * how about separating ioctls into two?
		 */
		bzero(oprl, sizeof(*oprl));
#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		pr = nd_prefix.lh_first;
		while (pr && i < PRLSTSIZ) {
			struct nd_pfxrouter *pfr;
			int j;

			(void)in6_embedscope(&oprl->prefix[i].prefix,
					     &pr->ndpr_prefix);
			oprl->prefix[i].raflags = pr->ndpr_raf;
			oprl->prefix[i].prefixlen = pr->ndpr_plen;
			oprl->prefix[i].vltime = pr->ndpr_vltime;
			oprl->prefix[i].pltime = pr->ndpr_pltime;
			oprl->prefix[i].if_index = pr->ndpr_ifp->if_index;
			if (pr->ndpr_vltime == ND6_INFINITE_LIFETIME)
				oprl->prefix[i].expire = 0;
			else {
				time_t maxexpire;

				/* XXX: we assume time_t is signed. */
				maxexpire = (-1) &
					~(1 << ((sizeof(maxexpire) * 8) - 1));
				if (pr->ndpr_vltime <
				    maxexpire - pr->ndpr_lastupdate) {
					oprl->prefix[i].expire =
						 pr->ndpr_lastupdate +
						pr->ndpr_vltime;
				} else
					oprl->prefix[i].expire = maxexpire;
			}
			pfr = pr->ndpr_advrtrs.lh_first;
			j = 0;
			while (pfr) {
				if (j < DRLSTSIZ) {
#define RTRADDR oprl->prefix[i].advrtr[j]
					RTRADDR = pfr->router->rtaddr;
					if (IN6_IS_ADDR_LINKLOCAL(&RTRADDR)) {
						/* XXX: hack for KAME */
						RTRADDR.s6_addr16[1] = 0;
					} else
						log(LOG_ERR,
						    "a router(%s) advertises "
						    "a prefix with "
						    "non-link local address\n",
						    ip6_sprintf(&RTRADDR));
#undef RTRADDR
				}
				j++;
				pfr = pfr->pfr_next;
			}
			oprl->prefix[i].advrtrs = j;
			oprl->prefix[i].origin = PR_ORIG_RA;

			i++;
			pr = pr->ndpr_next;
		}
		splx(s);

		break;
	case OSIOCGIFINFO_IN6:
		if (!nd_ifinfo || i >= nd_ifinfo_indexlim) {
			error = EINVAL;
			break;
		}
		ndi->ndi.linkmtu = nd_ifinfo[ifp->if_index].linkmtu;
		ndi->ndi.maxmtu = nd_ifinfo[ifp->if_index].maxmtu;
		ndi->ndi.basereachable =
		    nd_ifinfo[ifp->if_index].basereachable;
		ndi->ndi.reachable = nd_ifinfo[ifp->if_index].reachable;
		ndi->ndi.retrans = nd_ifinfo[ifp->if_index].retrans;
		ndi->ndi.flags = nd_ifinfo[ifp->if_index].flags;
		ndi->ndi.recalctm = nd_ifinfo[ifp->if_index].recalctm;
		ndi->ndi.chlim = nd_ifinfo[ifp->if_index].chlim;
		ndi->ndi.receivedra = nd_ifinfo[ifp->if_index].receivedra;
		break;
	case SIOCGIFINFO_IN6:
		if (!nd_ifinfo || i >= nd_ifinfo_indexlim) {
			error = EINVAL;
			break;
		}
		ndi->ndi = nd_ifinfo[ifp->if_index];
		break;
	case SIOCSIFINFO_FLAGS:
		/* XXX: almost all other fields of ndi->ndi is unused */
		if (!nd_ifinfo || i >= nd_ifinfo_indexlim) {
			error = EINVAL;
			break;
		}
		nd_ifinfo[ifp->if_index].flags = ndi->ndi.flags;
		break;
	case SIOCSNDFLUSH_IN6:	/* XXX: the ioctl name is confusing... */
		/* sync kernel routing table with the default router list */
		defrouter_reset();
		defrouter_select();
		break;
	case SIOCSPFXFLUSH_IN6:
	    {
		/* flush all the prefix advertised by routers */
		struct nd_prefix *pr, *next;

#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		for (pr = nd_prefix.lh_first; pr; pr = next) {
			struct in6_ifaddr *ia, *ia_next;

			next = pr->ndpr_next;

			if (IN6_IS_ADDR_LINKLOCAL(&pr->ndpr_prefix.sin6_addr))
				continue; /* XXX */

			/* do we really have to remove addresses as well? */
			for (ia = in6_ifaddr; ia; ia = ia_next) {
				/* ia might be removed.  keep the next ptr. */
				ia_next = ia->ia_next;

				if ((ia->ia6_flags & IN6_IFF_AUTOCONF) == 0)
					continue;

				if (ia->ia6_ndpr == pr)
					in6_purgeaddr(&ia->ia_ifa);
			}
			prelist_remove(pr);
		}
		splx(s);
		break;
	    }
	case SIOCSRTRFLUSH_IN6:
	    {
		/* flush all the default routers */
		struct nd_defrouter *dr, *next;

#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		defrouter_reset();
		for (dr = TAILQ_FIRST(&nd_defrouter); dr; dr = next) {
			next = TAILQ_NEXT(dr, dr_entry);
			defrtrlist_del(dr);
		}
		defrouter_select();
		splx(s);
		break;
	    }
	case SIOCGNBRINFO_IN6:
	    {
		struct llinfo_nd6 *ln;
		struct in6_addr nb_addr = nbi->addr; /* make local for safety */

		/*
		 * XXX: KAME specific hack for scoped addresses
		 *      XXXX: for other scopes than link-local?
		 */
		if (IN6_IS_ADDR_LINKLOCAL(&nbi->addr) ||
		    IN6_IS_ADDR_MC_LINKLOCAL(&nbi->addr)) {
			u_int16_t *idp = (u_int16_t *)&nb_addr.s6_addr[2];

			if (*idp == 0)
				*idp = htons(ifp->if_index);
		}

#ifdef __NetBSD__
		s = splsoftnet();
#else
		s = splnet();
#endif
		if ((rt = nd6_lookup(&nb_addr, 0, ifp)) == NULL ||
		    (ln = (struct llinfo_nd6 *)rt->rt_llinfo) == NULL) {
			error = EINVAL;
			splx(s);
			break;
		}
		nbi->state = ln->ln_state;
		nbi->asked = ln->ln_asked;
		nbi->isrouter = ln->ln_router;
		nbi->expire = ln->ln_expire;
		splx(s);
		
		break;
	    }
	case SIOCGDEFIFACE_IN6:	/* XXX: should be implemented as a sysctl? */
		ndif->ifindex = nd6_defifindex;
		break;
	case SIOCSDEFIFACE_IN6:	/* XXX: should be implemented as a sysctl? */
		return(nd6_setdefaultiface(ndif->ifindex));
		break;
	}
	return(error);
}

/*
 * Create neighbor cache entry and cache link-layer address,
 * on reception of inbound ND6 packets.  (RS/RA/NS/redirect)
 */
struct rtentry *
nd6_cache_lladdr(ifp, from, lladdr, lladdrlen, type, code)
	struct ifnet *ifp;
	struct in6_addr *from;
	char *lladdr;
	int lladdrlen;
	int type;	/* ICMP6 type */
	int code;	/* type dependent information */
{
	struct rtentry *rt = NULL;
	struct llinfo_nd6 *ln = NULL;
	int is_newentry;
	struct sockaddr_dl *sdl = NULL;
	int do_update;
	int olladdr;
	int llchange;
	int newstate = 0;
#if !(defined(__FreeBSD__) && __FreeBSD__ >= 3)
	long time_second = time.tv_sec;
#endif

	if (!ifp)
		panic("ifp == NULL in nd6_cache_lladdr");
	if (!from)
		panic("from == NULL in nd6_cache_lladdr");

	/* nothing must be updated for unspecified address */
	if (IN6_IS_ADDR_UNSPECIFIED(from))
		return NULL;

	/*
	 * Validation about ifp->if_addrlen and lladdrlen must be done in
	 * the caller.
	 *
	 * XXX If the link does not have link-layer adderss, what should
	 * we do? (ifp->if_addrlen == 0)
	 * Spec says nothing in sections for RA, RS and NA.  There's small
	 * description on it in NS section (RFC 2461 7.2.3).
	 */

	rt = nd6_lookup(from, 0, ifp);
	if (!rt) {
#if 0
		/* nothing must be done if there's no lladdr */
		if (!lladdr || !lladdrlen)
			return NULL;
#endif

		rt = nd6_lookup(from, 1, ifp);
		is_newentry = 1;
	} else {
		/* do nothing if static ndp is set */
		if (rt->rt_flags & RTF_STATIC)
			return NULL;
		is_newentry = 0;
	}

	if (!rt)
		return NULL;
	if ((rt->rt_flags & (RTF_GATEWAY | RTF_LLINFO)) != RTF_LLINFO) {
fail:
		(void)nd6_free(rt, 0);
		return NULL;
	}
	ln = (struct llinfo_nd6 *)rt->rt_llinfo;
	if (!ln)
		goto fail;
	if (!rt->rt_gateway)
		goto fail;
	if (rt->rt_gateway->sa_family != AF_LINK)
		goto fail;
	sdl = SDL(rt->rt_gateway);

	olladdr = (sdl->sdl_alen) ? 1 : 0;
	if (olladdr && lladdr) {
		if (bcmp(lladdr, LLADDR(sdl), ifp->if_addrlen))
			llchange = 1;
		else
			llchange = 0;
	} else
		llchange = 0;

	/*
	 * newentry olladdr  lladdr  llchange	(*=record)
	 *	0	n	n	--	(1)
	 *	0	y	n	--	(2)
	 *	0	n	y	--	(3) * STALE
	 *	0	y	y	n	(4) *
	 *	0	y	y	y	(5) * STALE
	 *	1	--	n	--	(6)   NOSTATE(= PASSIVE)
	 *	1	--	y	--	(7) * STALE
	 */

	if (lladdr) {		/* (3-5) and (7) */
		/*
		 * Record source link-layer address
		 * XXX is it dependent to ifp->if_type?
		 */
		sdl->sdl_alen = ifp->if_addrlen;
		bcopy(lladdr, LLADDR(sdl), ifp->if_addrlen);
	}

	if (!is_newentry) {
		if ((!olladdr && lladdr)		/* (3) */
		 || (olladdr && lladdr && llchange)) {	/* (5) */
			do_update = 1;
			newstate = ND6_LLINFO_STALE;
		} else					/* (1-2,4) */
			do_update = 0;
	} else {
		do_update = 1;
		if (!lladdr)				/* (6) */
			newstate = ND6_LLINFO_NOSTATE;
		else					/* (7) */
			newstate = ND6_LLINFO_STALE;
	}

	if (do_update) {
		/*
		 * Update the state of the neighbor cache.
		 */
		ln->ln_state = newstate;

		if (ln->ln_state == ND6_LLINFO_STALE) {
			/*
			 * XXX: since nd6_output() below will cause
			 * state tansition to DELAY and reset the timer,
			 * we must set the timer now, although it is actually
			 * meaningless.
			 */
			ln->ln_expire = time_second + nd6_gctimer;

			if (ln->ln_hold) {
				/*
				 * we assume ifp is not a p2p here, so just
				 * set the 2nd argument as the 1st one.
				 */
				nd6_output(ifp, ifp, ln->ln_hold,
					   (struct sockaddr_in6 *)rt_key(rt),
					   rt);
				ln->ln_hold = NULL;
			}
		} else if (ln->ln_state == ND6_LLINFO_INCOMPLETE) {
			/* probe right away */
			ln->ln_expire = time_second;
		}
	}

	/*
	 * ICMP6 type dependent behavior.
	 *
	 * NS: clear IsRouter if new entry
	 * RS: clear IsRouter
	 * RA: set IsRouter if there's lladdr
	 * redir: clear IsRouter if new entry
	 *
	 * RA case, (1):
	 * The spec says that we must set IsRouter in the following cases:
	 * - If lladdr exist, set IsRouter.  This means (1-5).
	 * - If it is old entry (!newentry), set IsRouter.  This means (7).
	 * So, based on the spec, in (1-5) and (7) cases we must set IsRouter.
	 * A quetion arises for (1) case.  (1) case has no lladdr in the
	 * neighbor cache, this is similar to (6).
	 * This case is rare but we figured that we MUST NOT set IsRouter.
	 *
	 * newentry olladdr  lladdr  llchange	    NS  RS  RA	redir
	 *							D R
	 *	0	n	n	--	(1)	c   ?     s
	 *	0	y	n	--	(2)	c   s     s
	 *	0	n	y	--	(3)	c   s     s
	 *	0	y	y	n	(4)	c   s     s
	 *	0	y	y	y	(5)	c   s     s
	 *	1	--	n	--	(6) c	c 	c s
	 *	1	--	y	--	(7) c	c   s	c s
	 *
	 *					(c=clear s=set)
	 */
	switch (type & 0xff) {
	case ND_NEIGHBOR_SOLICIT:
		/*
		 * New entry must have is_router flag cleared.
		 */
		if (is_newentry)	/* (6-7) */
			ln->ln_router = 0;
		break;
	case ND_REDIRECT:
		/*
		 * If the icmp is a redirect to a better router, always set the
		 * is_router flag.  Otherwise, if the entry is newly created,
		 * clear the flag.  [RFC 2461, sec 8.3]
		 */
		if (code == ND_REDIRECT_ROUTER)
			ln->ln_router = 1;
		else if (is_newentry) /* (6-7) */
			ln->ln_router = 0;
		break;
	case ND_ROUTER_SOLICIT:
		/*
		 * is_router flag must always be cleared.
		 */
		ln->ln_router = 0;
		break;
	case ND_ROUTER_ADVERT:
		/*
		 * Mark an entry with lladdr as a router.
		 */
		if ((!is_newentry && (olladdr || lladdr))	/* (2-5) */
		 || (is_newentry && lladdr)) {			/* (7) */
			ln->ln_router = 1;
		}
		break;
	}

	/*
	 * When the link-layer address of a router changes, select the
	 * best router again.  In particular, when the neighbor entry is newly
	 * created, it might affect the selection policy.
	 * Question: can we restrict the first condition to the "is_newentry"
	 * case?
	 * XXX: when we hear an RA from a new router with the link-layer
	 * address option, defrouter_select() is called twice, since
	 * defrtrlist_update called the function as well.  However, I believe
	 * we can compromise the overhead, since it only happens the first
	 * time.
	 * XXX: although defrouter_select() should not have a bad effect
	 * for those are not autoconfigured hosts, we explicitly avoid such
	 * cases for safety.
	 */
	if (do_update && ln->ln_router && !ip6_forwarding && ip6_accept_rtadv)
		defrouter_select();

	return rt;
}

static void
nd6_slowtimo(ignored_arg)
    void *ignored_arg;
{
#ifdef __NetBSD__
	int s = splsoftnet();
#else
	int s = splnet();
#endif
	int i;
	struct nd_ifinfo *nd6if;

#if defined(__NetBSD__) || (defined(__FreeBSD__) && __FreeBSD__ >= 3)
	callout_reset(&nd6_slowtimo_ch, ND6_SLOWTIMER_INTERVAL * hz,
	    nd6_slowtimo, NULL);
#elif defined(__OpenBSD__)
	timeout_set(&nd6_slowtimo_ch, nd6_slowtimo, NULL);
	timeout_add(&nd6_slowtimo_ch, ND6_SLOWTIMER_INTERVAL * hz);
#else
	timeout(nd6_slowtimo, (caddr_t)0, ND6_SLOWTIMER_INTERVAL * hz);
#endif
	for (i = 1; i < if_index + 1; i++) {
		if (!nd_ifinfo || i >= nd_ifinfo_indexlim)
			continue;
		nd6if = &nd_ifinfo[i];
		if (nd6if->basereachable && /* already initialized */
		    (nd6if->recalctm -= ND6_SLOWTIMER_INTERVAL) <= 0) {
			/*
			 * Since reachable time rarely changes by router
			 * advertisements, we SHOULD insure that a new random
			 * value gets recomputed at least once every few hours.
			 * (RFC 2461, 6.3.4)
			 */
			nd6if->recalctm = nd6_recalc_reachtm_interval;
			nd6if->reachable = ND_COMPUTE_RTIME(nd6if->basereachable);
		}
	}
	splx(s);
}

#define senderr(e) { error = (e); goto bad;}
int
nd6_output(ifp, origifp, m0, dst, rt0)
	struct ifnet *ifp;
	struct ifnet *origifp;
	struct mbuf *m0;
	struct sockaddr_in6 *dst;
	struct rtentry *rt0;
{


	struct mbuf *m = m0;
	struct rtentry *rt = rt0;
	struct sockaddr_in6 *gw6 = NULL;
	struct llinfo_nd6 *ln = NULL;
	int error = 0;
#if !(defined(__FreeBSD__) && __FreeBSD__ >= 3)
	long time_second = time.tv_sec;
#endif
#if defined(__OpenBSD__) && defined(IPSEC)
	struct m_tag *mtag;
#endif /* IPSEC */

	if (IN6_IS_ADDR_MULTICAST(&dst->sin6_addr))
		goto sendpkt;

	if (nd6_need_cache(ifp) == 0)
		goto sendpkt;

	/*
	 * next hop determination.  This routine is derived from ether_outpout.
	 */
	if (rt) {
		if ((rt->rt_flags & RTF_UP) == 0) {
#ifdef __FreeBSD__
			if ((rt0 = rt = rtalloc1((struct sockaddr *)dst, 1, 0UL)) !=
				NULL)
#else
			if ((rt0 = rt = rtalloc1((struct sockaddr *)dst, 1)) !=
				NULL)
#endif
			{
				rt->rt_refcnt--;
				if (rt->rt_ifp != ifp) {
					/* XXX: loop care? */
					return nd6_output(ifp, origifp, m0,
							  dst, rt);
				}
			} else
                senderr(EHOSTUNREACH);
		}

		if (rt->rt_flags & RTF_GATEWAY) {
			gw6 = (struct sockaddr_in6 *)rt->rt_gateway;

			/*
			 * We skip link-layer address resolution and NUD
			 * if the gateway is not a neighbor from ND point
			 * of view, regardless of the value of nd_ifinfo.flags.
			 * The second condition is a bit tricky; we skip
			 * if the gateway is our own address, which is
			 * sometimes used to install a route to a p2p link.
			 */
			if (!nd6_is_addr_neighbor(gw6, ifp) ||
			    in6ifa_ifpwithaddr(ifp, &gw6->sin6_addr)) {

				/*
				 * We allow this kind of tricky route only
				 * when the outgoing interface is p2p.
				 * XXX: we may need a more generic rule here.
				 */
				if ((ifp->if_flags & IFF_POINTOPOINT) == 0)
                    senderr(EHOSTUNREACH);

				goto sendpkt;
			}

			if (rt->rt_gwroute == 0)
				goto lookup;
			if (((rt = rt->rt_gwroute)->rt_flags & RTF_UP) == 0) {
				rtfree(rt); rt = rt0;
			lookup:
#ifdef __FreeBSD__
				rt->rt_gwroute = rtalloc1(rt->rt_gateway, 1, 0UL);
#else
				rt->rt_gwroute = rtalloc1(rt->rt_gateway, 1);
#endif
				if ((rt = rt->rt_gwroute) == 0)
                    senderr(EHOSTUNREACH);
#if defined(__bsdi__) || defined(__NetBSD__)
				/* the "G" test below also prevents rt == rt0 */
				if ((rt->rt_flags & RTF_GATEWAY) ||
				    (rt->rt_ifp != ifp)) {
					rt->rt_refcnt--;
					rt0->rt_gwroute = 0;
                        senderr(EHOSTUNREACH);
				}
#endif
			}
		}
	}

	/*
	 * Address resolution or Neighbor Unreachability Detection
	 * for the next hop.
	 * At this point, the destination of the packet must be a unicast
	 * or an anycast address(i.e. not a multicast).
	 */

	/* Look up the neighbor cache for the nexthop */
	if (rt && (rt->rt_flags & RTF_LLINFO) != 0)
		ln = (struct llinfo_nd6 *)rt->rt_llinfo;
	else {
		/*
		 * Since nd6_is_addr_neighbor() internally calls nd6_lookup(),
		 * the condition below is not very efficient.  But we believe
		 * it is tolerable, because this should be a rare case.
		 */
		if (nd6_is_addr_neighbor(dst, ifp) &&
		    (rt = nd6_lookup(&dst->sin6_addr, 1, ifp)) != NULL)
			ln = (struct llinfo_nd6 *)rt->rt_llinfo;
	}
	if (!ln || !rt) {
		if ((ifp->if_flags & IFF_POINTOPOINT) == 0 &&
		    !(nd_ifinfo[ifp->if_index].flags & ND6_IFF_PERFORMNUD)) {
			log(LOG_DEBUG,
			    "nd6_output: can't allocate llinfo for %s "
			    "(ln=%p, rt=%p)\n",
			    ip6_sprintf(&dst->sin6_addr), ln, rt);
			senderr(EIO);	/* XXX: good error? */
		}

		goto sendpkt;	/* send anyway */
	}

	/* We don't have to do link-layer address resolution on a p2p link. */
	if ((ifp->if_flags & IFF_POINTOPOINT) != 0 &&
	    ln->ln_state < ND6_LLINFO_REACHABLE) {
		ln->ln_state = ND6_LLINFO_STALE;
		ln->ln_expire = time_second + nd6_gctimer;
	}

	/*
	 * The first time we send a packet to a neighbor whose entry is
	 * STALE, we have to change the state to DELAY and a sets a timer to
	 * expire in DELAY_FIRST_PROBE_TIME seconds to ensure do
	 * neighbor unreachability detection on expiration.
	 * (RFC 2461 7.3.3)
	 */
	if (ln->ln_state == ND6_LLINFO_STALE) {
		ln->ln_asked = 0;
		ln->ln_state = ND6_LLINFO_DELAY;
		ln->ln_expire = time_second + nd6_delay;
	}

	/*
	 * If the neighbor cache entry has a state other than INCOMPLETE
	 * (i.e. its link-layer address is already resolved), just
	 * send the packet.
	 */
	if (ln->ln_state > ND6_LLINFO_INCOMPLETE)
		goto sendpkt;

	/*
	 * There is a neighbor cache entry, but no ethernet address
	 * response yet.  Replace the held mbuf (if any) with this
	 * latest one.
	 * This code conforms to the rate-limiting rule described in Section
	 * 7.2.2 of RFC 2461, because the timer is set correctly after sending
	 * an NS below.
	 */
	if (ln->ln_state == ND6_LLINFO_NOSTATE)
		ln->ln_state = ND6_LLINFO_INCOMPLETE;
	if (ln->ln_hold)
		m_freem(ln->ln_hold);
	ln->ln_hold = m;
	if (ln->ln_expire) {
		if (ln->ln_asked < nd6_mmaxtries &&
		    ln->ln_expire < time_second) {
			ln->ln_asked++;
			ln->ln_expire = time_second +
				nd_ifinfo[ifp->if_index].retrans / 1000;
			nd6_ns_output(ifp, NULL, &dst->sin6_addr, ln, 0);
		}
	}
	return(0);
	
  sendpkt:
#if defined(__OpenBSD__) && defined(IPSEC)
	/*
	 * If the packet needs outgoing IPsec crypto processing and the
	 * interface doesn't support it, drop it.
	 */
	mtag = m_tag_find(m, PACKET_TAG_IPSEC_OUT_CRYPTO_NEEDED, NULL);
#endif /* IPSEC */

	if ((ifp->if_flags & IFF_LOOPBACK) != 0) {
#if defined(__OpenBSD__) && defined(IPSEC)
		if (mtag != NULL &&
		    (origifp->if_capabilities & IFCAP_IPSEC) == 0) {
			/* Tell IPsec to do its own crypto. */
			ipsp_skipcrypto_unmark((struct tdb_ident *)(mtag + 1));
			error = EACCES;
			goto bad;
		}
#endif /* IPSEC */
		return((*ifp->if_output)(origifp, m, (struct sockaddr *)dst,
					 rt));
	}
#if defined(__OpenBSD__) && defined(IPSEC)
	if (mtag != NULL &&
	    (ifp->if_capabilities & IFCAP_IPSEC) == 0) {
		/* Tell IPsec to do its own crypto. */
		ipsp_skipcrypto_unmark((struct tdb_ident *)(mtag + 1));
		error = EACCES;
		goto bad;
	}
#endif /* IPSEC */
	return((*ifp->if_output)(ifp, m, (struct sockaddr *)dst, rt));

  bad:
	if (m)
		m_freem(m);
	return (error);
}	
#undef senderr

int
nd6_need_cache(ifp)
	struct ifnet *ifp;
{
	/*
	 * XXX: we currently do not make neighbor cache on any interface
	 * other than ARCnet, Ethernet, FDDI and GIF.
	 *
	 * RFC2893 says:
	 * - unidirectional tunnels needs no ND
	 */
	switch (ifp->if_type) {
	case IFT_ARCNET:
	case IFT_ETHER:
	case IFT_FDDI:
	case IFT_IEEE1394:
#ifdef IFT_PROPVIRTUAL
	case IFT_PROPVIRTUAL:
#endif
#ifdef IFT_L2VLAN
	case IFT_L2VLAN:
#endif
#ifdef IFT_IEEE80211
	case IFT_IEEE80211:
#endif
	case IFT_GIF:		/* XXX need more cases? */
		return(1);
	default:
		return(0);
	}
}

int
nd6_storelladdr(ifp, rt, m, dst, desten)
	struct ifnet *ifp;
	struct rtentry *rt;
	struct mbuf *m;
	struct sockaddr *dst;
	u_char *desten;
{
	int i;
	struct sockaddr_dl *sdl;

	if (m->m_flags & M_MCAST) {
		switch (ifp->if_type) {
		case IFT_ETHER:
		case IFT_FDDI:
#ifdef IFT_PROPVIRTUAL
		case IFT_PROPVIRTUAL:
#endif
#ifdef IFT_L2VLAN
		case IFT_L2VLAN:
#endif
#ifdef IFT_IEEE80211
		case IFT_IEEE80211:
#endif
			ETHER_MAP_IPV6_MULTICAST(&SIN6(dst)->sin6_addr,
						 desten);
			return(1);
		case IFT_IEEE1394:
			/*
			 * netbsd can use if_broadcastaddr, but we don't do so
			 * to reduce # of ifdef.
			 */
			for (i = 0; i < ifp->if_addrlen; i++)
				desten[i] = ~0;
			return(1);
		case IFT_ARCNET:
			*desten = 0;
			return(1);
		default:
			m_freem(m);
			return(0);
		}
	}

	if (rt == NULL) {
		/* this could happen, if we could not allocate memory */
		m_freem(m);
		return(0);
	}
	if (rt->rt_gateway->sa_family != AF_LINK) {
		m_freem(m);
		return(0);
	}
	sdl = SDL(rt->rt_gateway);
	if (sdl->sdl_alen == 0) {
		/* this should be impossible, but we bark here for debugging */
		printf("nd6_storelladdr: sdl_alen == 0, dst=%s, if=%s\n",
		       ip6_sprintf(&SIN6(dst)->sin6_addr), if_name(ifp));
		m_freem(m);
		return(0);
	}

	bcopy(LLADDR(sdl), desten, sdl->sdl_alen);
	return(1);
}
