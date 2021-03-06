/*
 *	Generic parts
 *	Linux ethernet Rbridge
 *
 *	Authors:
 *	Ahmed AMAMOU	<ahmed@gandi.net>
 *	Kamel Haddadou	<kamel@gandi.net>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include "rbr_private.h"

static struct rbr *add_rbr(struct net_bridge *br)
{
	struct rbr *rbr;

	if (!br->rbr) {
		rbr = kzalloc(sizeof(*rbr), GFP_KERNEL);
		if (!rbr)
			return NULL;

		rbr->br = br;
		rbr->nick = RBRIDGE_NICKNAME_NONE;
		rbr->treeroot = RBRIDGE_NICKNAME_NONE;
		return rbr;
	}

	return br->rbr;
}

static void rbr_del_node(struct rbr *rbr, uint16_t nickname)
{
	struct rbr_node *rbr_node;

	if (likely(VALID_NICK(nickname))) {
		rbr_node = rbr->rbr_nodes[nickname];
		if (likely(rbr_node != NULL)) {
			rcu_assign_pointer(rbr->rbr_nodes[nickname], NULL);
			rbr_node_put(rbr_node);
		}
	}
}

static void rbr_del_all(struct rbr *rbr)
{
	unsigned int i;

	for (i = RBRIDGE_NICKNAME_MIN; i < RBRIDGE_NICKNAME_MAX; i++) {
		if (likely(rbr->rbr_nodes[i] != NULL))
			rbr_del_node(rbr, i);
	}
}

static void br_trill_start(struct net_bridge *br)
{
	/* Disable STP if it is already enabled */
	if (br->stp_enabled != BR_NO_STP)
	  br_stp_stop(br);
	br->rbr = add_rbr(br);
	if (br->rbr) {
		spin_lock_bh(&br->lock);
		br->trill_enabled = BR_TRILL;
		spin_unlock_bh(&br->lock);
	} else {
		printk(KERN_WARNING "RBridge allocation for bridge '%s' failed\n",
		       br->dev->name);
	}
}

static void br_trill_stop(struct net_bridge *br)
{
	struct rbr *old;

	spin_lock_bh(&br->lock);
	br->trill_enabled = BR_NO_TRILL;
	spin_unlock_bh(&br->lock);
	old = br->rbr;
	br->rbr = NULL;
	if (likely(old)) {
		spin_lock_bh(&br->lock);
		rbr_del_all(old);
		kfree(old);
		spin_unlock_bh(&br->lock);
	}
}

int set_treeroot(struct rbr *rbr, uint16_t treeroot)
{
	if (unlikely(!VALID_NICK(treeroot))) {
		pr_warn_ratelimited("rbr_set_treeroot: given tree root not valid\n");
		goto set_tree_root_fail;
	}
	if (rbr->treeroot != treeroot) {
		spin_lock_bh(&rbr->br->lock);
		rbr->treeroot = treeroot;
		spin_unlock_bh(&rbr->br->lock);
	}

	return 0;

set_tree_root_fail:
	return ENOENT;
}

struct rbr_node *rbr_find_node(struct rbr *rbr, __u16 nickname)
{
	struct rbr_node *rbr_node;

	if (unlikely(!VALID_NICK(nickname)))
		return NULL;

	rbr_node = rcu_dereference(rbr->rbr_nodes[nickname]);
	rbr_node_get(rbr_node);

	return rbr_node;
}

void br_trill_set_enabled(struct net_bridge *br, unsigned long val)
{
	if (val) {
		if (br->trill_enabled == BR_NO_TRILL)
			br_trill_start(br);
	} else {
		if (br->trill_enabled != BR_NO_TRILL)
			br_trill_stop(br);
	}
}

static void rbr_fwd_finish(struct sk_buff *skb, u16 vid)
{
	struct net_bridge *br;
	struct net_bridge_fdb_entry *dst;
	struct ethhdr *outerethhdr;
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	struct net_device *dev = skb->dev;

	outerethhdr = eth_hdr(skb);
	br = netdev_priv(dev);
	dst = __br_fdb_get(br, dest, vid);
	if (likely(dst)) {
		dst->used = jiffies;
		memcpy(outerethhdr->h_source, dst->dst->dev->dev_addr,
		       dst->dst->dev->addr_len);
		br_forward(dst->dst, skb, NULL);
	} else {
		br_trill_flood_forward(br, skb, NULL);
	}
}

static void rbr_fwd(struct net_bridge_port *p, struct sk_buff *skb,
		    uint16_t adj_nick, u16 vid)
{
	struct rbr_node *adj;
	struct trill_hdr *trh;
	struct ethhdr *outerethhdr;

	adj = rbr_find_node(p->br->rbr, adj_nick);
	if (unlikely(adj == NULL)) {
		pr_warn_ratelimited("rbr_fwd: unable to find adjacent RBridge\n");
		goto dest_fwd_fail;
	}

	trh = (struct trill_hdr *) skb->data;
	trillhdr_dec_hopcount(trh);
	outerethhdr = eth_hdr(skb);

	/* change outer ether header */
	/* bridge become the source_port address in outeretherhdr */
	memcpy(outerethhdr->h_source, p->br->dev->dev_addr, ETH_ALEN);
	/* dist port become dest address in outeretherhdr */
	memcpy(outerethhdr->h_dest, adj->rbr_ni->adjsnpa, ETH_ALEN);
	rbr_node_put(adj);
	/* set Bridge as source device */
	skb->dev = p->br->dev;
	rbr_fwd_finish(skb, vid);

	return;

dest_fwd_fail:
	if (likely(p && p->br))
		p->br->dev->stats.tx_dropped++;
	kfree_skb(skb);
	return;
}

static int rbr_multidest_fwd(struct net_bridge_port *p,
			     struct sk_buff *skb, uint16_t egressnick,
			     uint16_t ingressnick, const uint8_t *saddr,
			     u16 vid, bool free)
{
	struct rbr *rbr;
	struct rbr_node *dest;
	struct rbr_node *adj;
	struct sk_buff *skb2;
	uint16_t adjnicksaved = 0;
	uint16_t adjnick;
	bool nicksaved = false;
	unsigned int i;

	if (unlikely(!p)) {
		pr_warn_ratelimited("rbr_multidest_fwd:port error\n");
		goto multidest_fwd_fail;
	}

	rbr = p->br->rbr;
	if (unlikely(rbr == NULL))
		goto multidest_fwd_fail;

	/* Lookup the egress nick info, this is the DT root */
	if ((dest = rbr_find_node(rbr, egressnick)) == NULL) {
		pr_warn_ratelimited("rbr_multidest_fwd: unable to find egress\n");
		goto multidest_fwd_fail;
	}

	/* Send a copy to all our adjacencies on the DT root */
	for (i = 0; i < dest->rbr_ni->adjcount; i++) {
		/* Check for a valid adjacency node */
		adjnick = RBR_NI_ADJNICK(dest->rbr_ni, i);
		if (!VALID_NICK(adjnick) || ingressnick == adjnick ||
		    ((adj = rbr_find_node(rbr, adjnick)) == NULL))
			continue;
		/* Do not forward back to adjacency that sent the pkt to us */
		if ((saddr != NULL) &&
		    (memcmp(adj->rbr_ni->adjsnpa, saddr, ETH_ALEN) == 0)) {
			rbr_node_put(adj);
			continue;
		}

		/* save the first found adjacency to avoid coping SKB
		 * if no other adjacency is found later no frame copy will be made
		 * if other adjacency will be found frame will be copied
		 * and forwarded to them if skb is needed after rbr_multidest_fwd
		 * copy of the first skb skb will be forced
		 */
		if (!nicksaved && free) {
			adjnicksaved = adjnick;
			nicksaved = true;
			rbr_node_put(adj);
			continue;
		}
		/* FIXME using copy instead of clone as
		 * we are going to modify dest adress
		 */
		if (unlikely((skb2 = skb_copy(skb, GFP_ATOMIC)) == NULL)) {
			p->br->dev->stats.tx_dropped++;
			pr_warn_ratelimited("rbr_multidest_fwd:skb_copy failed\n");
			goto multidest_fwd_fail;
		}
		rbr_fwd(p, skb2, adjnick, vid);
		rbr_node_put(adj);
	}
	rbr_node_put(dest);

	/* if nicksave is false it means that copy will not be forwarded
	 * as no availeble ajacency was found in such a case frame should
	 * be dropped
	 */

	if (nicksaved)
		rbr_fwd(p, skb, adjnicksaved, vid);
	else
		kfree_skb(skb);

	return 0;

multidest_fwd_fail:
	if (likely(p && p->br))
		p->br->dev->stats.tx_dropped++;
	kfree_skb(skb);
	return -1;
}

static bool rbr_encaps(struct sk_buff *skb, uint16_t ingressnick,
		       uint16_t egressnick, bool multidest)
{
	struct trill_hdr *trh;
	size_t trhsize;
	u16 vlan_tci;
	u16 trill_flags = 0;
	#ifdef CONFIG_TRILL_VNT
	struct trill_opt *trill_opt;
	struct trill_vnt_extension *vnt;
	struct net_bridge_port *p;
	uint32_t vni = 0;
	u16 vnt_flags = 0;
	u32 opt_flows = 0;
	u32 opt_flags = 0;
	#endif
	trhsize = sizeof(*trh);
	#ifdef CONFIG_TRILL_VNT
	p = br_port_get_rcu(skb->dev);
	if (likely(p))
		vni = get_port_vni_id(p);
	if (likely(vni))
		trhsize += sizeof(struct trill_opt) + sizeof(struct trill_vnt_extension);
	#endif
	skb_push(skb, ETH_HLEN);
	if (!skb->encapsulation) {
		skb_reset_inner_headers(skb);
		skb->encapsulation = 1;
	}
	if (br_vlan_get_tag(skb, &vlan_tci) == 0) {
		skb = vlan_insert_tag(skb, skb->vlan_proto, vlan_tci);
		if (skb == NULL) {
			printk(KERN_ERR "rbr_encaps: vlan_put_tag failed\n");
			return 1;
		}
		skb->vlan_proto = 0;
		skb->vlan_tci = 0;
	}
	if (unlikely(skb_cow_head(skb, trhsize + ETH_HLEN))) {
		printk(KERN_ERR "rbr_encaps: cow_head failed\n");
		return 1;
	}
	#ifdef CONFIG_TRILL_VNT
	if (likely(vni)) {
		vnt = (struct trill_vnt_extension *) skb_push(skb, sizeof(*vnt));
		trill_opt = (struct trill_opt *) skb_push(skb, sizeof(*trill_opt));
		/* opt_flags to be defined later */
		trill_opt->opt_flag = htonl(opt_flags);
		/* opt_flows will be used for multipath */
		trill_opt->opt_flow = htonl(opt_flows);
		vnt_flags = vnt_flags |
			trill_extension_set_app(0) |
			trill_extension_set_nc(0) |
			trill_extension_set_type(VNT_EXTENSION_TYPE) |
			trill_extension_set_mu(0) |
			trill_extension_set_length(VNT_EXTENSION_LENGTH);
		vnt->flags = htons(vnt_flags);
		vnt->reserved_high = htons(0);
		trill_extension_set_vni(vnt, vni_to_network(vni));
		trill_flags = trill_set_optslen(sizeof(*trill_opt) + sizeof(*vnt));
	}
	#endif
	trh = (struct trill_hdr *) skb_push(skb, sizeof(*trh));
	trill_flags = trill_flags |
			trill_set_version(TRILL_PROTOCOL_VERS) |
			trill_set_hopcount(TRILL_DEFAULT_HOPS) |
			trill_set_multidest(multidest ? 1 : 0);
	trh->th_flags = htons(trill_flags);
	trh->th_egressnick = egressnick;
	trh->th_ingressnick = ingressnick; /* self nick name */
	skb_push(skb, ETH_HLEN); /* make skb->mac_header point to outer mac header */
	skb_reset_mac_header(skb); /* instead of the inner one */
	eth_hdr(skb)->h_proto = __constant_htons(ETH_P_TRILL);
	/* make skb->data point to the right place (just after ether header) */
	skb_pull(skb, ETH_HLEN);
	skb_reset_mac_len(skb);
	return 0;
}

static void rbr_encaps_prepare(struct sk_buff *skb, uint16_t egressnick,
			       u16 vid)
{
	uint16_t local_nick;
	uint16_t dtrNick;
	struct rbr_node *self;
	struct sk_buff *skb2;
	struct rbr *rbr;
	struct net_bridge_port *p;
#ifdef CONFIG_TRILL_VNT
	struct vni *vni;
	uint32_t vni_id;
#endif
	p = br_port_get_rcu(skb->dev);
	if (unlikely(!p)) {
		pr_warn_ratelimited("rbr_encaps_prepare: port error\n");
		goto encaps_drop;
	}
	rbr = p->br->rbr;

	if (unlikely(egressnick != RBRIDGE_NICKNAME_NONE
         && !VALID_NICK(egressnick))) {
		pr_warn_ratelimited("rbr_encaps_prepare: invalid destinaton nickname\n");
		goto encaps_drop;
	}
	local_nick = rbr->nick;
	if (unlikely(!VALID_NICK(local_nick))) {
		pr_warn_ratelimited("rbr_encaps_prepare: invalid local nickname\n");
		goto encaps_drop;
	}
	if (egressnick == RBRIDGE_NICKNAME_NONE) {
		/* Daemon has not yet sent the local nickname */
		if (unlikely((self = rbr_find_node(rbr, local_nick)) == NULL)) {
			pr_warn_ratelimited("rbr_encaps_prepare: waiting for nickname\n");
			goto encaps_drop;
		}
		if (self->rbr_ni->dtrootcount > 0)
			dtrNick = RBR_NI_DTROOTNICK(self->rbr_ni, 0);
		else
			dtrNick = rbr->treeroot;
		rbr_node_put(self);
		if (unlikely(!VALID_NICK(dtrNick))) {
			pr_warn_ratelimited("rbr_encaps_prepare: dtrNick is unvalid\n");
			goto encaps_drop;
		}
		if (unlikely((skb2 = skb_clone(skb, GFP_ATOMIC)) == NULL)) {
			p->br->dev->stats.tx_dropped++;
			pr_warn_ratelimited("rbr_encaps_prepare: skb_clone failed\n");
			goto encaps_drop;
		}
#ifdef CONFIG_TRILL_VNT
		vni_id = get_port_vni_id(p);
		if (vni_id) {
			vni = find_vni(p->br, vni_id);
			vni_flood_deliver(vni, skb2, FREE_SKB);
		}
		else
#endif
		br_endstation_deliver(p->br, skb2);
		if (unlikely(rbr_encaps(skb, local_nick, dtrNick, 1)))
			goto encaps_drop;
		rbr_multidest_fwd(p, skb, dtrNick, local_nick, NULL, vid, true);
	} else {
		if (unlikely(rbr_encaps(skb, local_nick, egressnick, 0)))
			goto encaps_drop;
		rbr_fwd(p, skb, egressnick, vid);
	}

	return;

encaps_drop:
	if (likely(p && p->br))
		p->br->dev->stats.tx_dropped++;
	kfree_skb(skb);
	return;
}

#ifdef CONFIG_TRILL_VNT
static void rbr_decap_finish(struct sk_buff *skb, u16 vid,
				     uint32_t vni)
#else
static void rbr_decap_finish(struct sk_buff *skb, u16 vid)
#endif
{
	struct net_bridge *br;
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	struct net_bridge_fdb_entry *dst;
	struct net_device *dev = skb->dev;

	br = netdev_priv(dev);
	dst = __br_fdb_get(br, dest, vid);
	if (likely(dst)) {
	#ifdef CONFIG_TRILL_VNT
		if (get_port_vni_id(dst->dst) != vni)
			goto rbr_decap_finish_drop;
		else
	#endif
			br_deliver(dst->dst, skb);
	} else {
		#ifdef CONFIG_TRILL_VNT
		if (likely(vni)) {
			struct vni *VNI;
			if (likely((VNI = find_vni(br, vni)) && VNI))
				vni_flood_deliver(VNI, skb, FREE_SKB);
			else
				goto rbr_decap_finish_drop;
		} else
		#endif
		{
			do {
				br_endstation_deliver(br, skb);
			} while(0);
		}
	}
	return;
#ifdef CONFIG_TRILL_VNT
rbr_decap_finish_drop:
#endif
	if (likely(br))
		br->dev->stats.rx_dropped++;
	kfree_skb(skb);
}

static void rbr_decaps(struct net_bridge_port *p,
		       struct sk_buff *skb,
		       size_t trhsize, u16 vid)
{
	struct trill_hdr *trh;
	struct ethhdr *hdr;
#ifdef CONFIG_TRILL_VNT
	uint32_t vni = 0;
#endif

	if (unlikely(p == NULL))
		goto rbr_decaps_drop;
	trh = (struct trill_hdr *)skb->data;
	if (trhsize >= sizeof(*trh))
		skb_pull(skb, sizeof(*trh));
	else
		goto rbr_decaps_drop;
	trhsize -= sizeof(*trh);
#ifdef CONFIG_TRILL_VNT
	if (likely(trill_get_optslen(ntohs(trh->th_flags)))) {
		struct trill_vnt_extension *vnt;
		if (trhsize > sizeof(struct trill_opt))
			skb_pull(skb, sizeof(struct trill_opt));
		else
			goto rbr_decaps_drop;
		trhsize -= sizeof(struct trill_opt);
		vnt = (struct trill_vnt_extension *) skb->data;
		 if (unlikely(trill_extension_get_type(vnt->flags !=
		   VNT_EXTENSION_TYPE))) {
			kfree_skb(skb);
			return;
		}
		vni = network_to_vni((uint32_t)trill_extension_get_vni(vnt));
		if (trhsize >= sizeof(*vnt))
			skb_pull(skb, sizeof(*vnt));
		else
			goto rbr_decaps_drop;
		trhsize -= sizeof(*vnt);
		if (unlikely(trhsize > 0)) {
			pr_warn_ratelimited("unknown option encountred dropping frame for safty\n");
			goto rbr_decaps_drop;
		}
	}
#endif
	skb_reset_mac_header(skb);  /* instead of the inner one */
	skb->protocol = eth_hdr(skb)->h_proto;
	hdr = (struct ethhdr*)skb->data;
	skb_pull(skb, ETH_HLEN);
	skb_reset_network_header(skb);
	if (skb->encapsulation)
		skb->encapsulation = 0;
	/* Mark bridge as source device */
	skb->dev = p->br->dev;
	br_fdb_update_nick(p->br, p, hdr->h_source, vid, trh->th_ingressnick);
#ifdef CONFIG_TRILL_VNT
	rbr_decap_finish(skb, vid, vni);
#else
	rbr_decap_finish(skb, vid);
#endif
	return;
rbr_decaps_drop:
	if (likely(p && p->br))
		p->br->dev->stats.rx_dropped++;
	kfree_skb(skb);
}

static void rbr_recv(struct sk_buff *skb, u16 vid)
{
	uint16_t local_nick, dtrNick, adjnick, idx;
	struct rbr *rbr;
	uint8_t srcaddr[ETH_ALEN];
	struct trill_hdr *trh;
	size_t trhsize;
	struct net_bridge_port *p;
	u16 trill_flags;
	struct sk_buff *skb2;
	struct rbr_node *dest = NULL;
	struct rbr_node *source_node = NULL;
	struct rbr_node *adj = NULL;

	p = br_port_get_rcu(skb->dev);
	if (unlikely(!p)) {
		pr_warn_ratelimited("rbr_recv: port error\n");
		goto recv_drop;
	} else {
		rbr = p->br->rbr;
	}
	/* For trill frame the outer mac destination must correspond to localhost
	 * address, if not frame must be discarded
	 * such scenario is possible when switch flood frames on all ports
	 * if frame are not discarded they will loop until reaching the
	 * hop_count limit
	 */
	if (!memcmp(p->dev->dev_addr, eth_hdr(skb)->h_dest, ETH_ALEN) == 0)
	  goto recv_drop;
	memcpy(srcaddr, eth_hdr(skb)->h_source, ETH_ALEN);
	trh = (struct trill_hdr *)skb->data;
	trill_flags = ntohs(trh->th_flags);
	trhsize = sizeof(*trh) + trill_get_optslen(trill_flags);
	if (unlikely(skb->len < trhsize + ETH_HLEN)) {
		pr_warn_ratelimited("rbr_recv:sk_buff len is less then minimal len\n");
		goto recv_drop;
	}
       /*
	* seems to be a valid TRILL frame,
	* check if TRILL header can be pulled
	* before proceeding
	*/
	if (unlikely(!pskb_may_pull(skb, trhsize + ETH_HLEN)))
		goto recv_drop;
	/*
	* WARNING SKB structure may be changed by pskb_may_pull
	* reassign trh pointer before continuing any further
	*/
	trh = (struct trill_hdr *)skb->data;

	if (!skb->encapsulation) {
		skb_pull(skb, trhsize + ETH_HLEN);
		skb_reset_inner_headers(skb);
		skb->encapsulation = 1;
		skb_push(skb, trhsize + ETH_HLEN);
	}
	if (unlikely(!VALID_NICK(trh->th_ingressnick) ||
           !VALID_NICK(trh->th_egressnick))) {
		pr_warn_ratelimited("rbr_recv: invalid nickname\n");
		goto recv_drop;
	}
	if (unlikely(trill_get_version(trill_flags) != TRILL_PROTOCOL_VERS)) {
		pr_warn_ratelimited("rbr_recv: not the same trill version\n");
		goto recv_drop;
	}
	local_nick = rbr->nick;
	dtrNick = rbr->treeroot;
	if (unlikely(trh->th_ingressnick == local_nick)) {
		pr_warn_ratelimited("rbr_recv:looping back frame check your config\n");
		goto recv_drop;
	}

#ifndef CONFIG_TRILL_VNT
	if (unlikely(trill_get_optslen(trill_flags))) {
		pr_warn_ratelimited("Found unknown TRILL header extension\n");
		goto recv_drop;
	}
#endif

	if (!trill_get_multidest(trill_flags)) {
		/* ntohs not needed as the 2 are in the same bit form */
		if (trh->th_egressnick == trh->th_ingressnick) {
			pr_warn_ratelimited("rbr_recv: egressnick == ingressnick\n");
			goto recv_drop;
		}
		if (trh->th_egressnick == local_nick) {
			rbr_decaps(p, skb, trhsize, vid);
		} else if (likely(trill_get_hopcount(trill_flags))) {
			br_fdb_update(p->br, p, srcaddr, vid);
			rbr_fwd(p, skb, trh->th_egressnick, vid);
		} else {
			pr_warn_ratelimited("rbr_recv: hop count limit reached\n");
			goto recv_drop;
		}
		return;
	}

	 /* Multi-destination frame:
	 * Check if received  multi-destination frame from an
	 * adjacency in the distribution tree rooted at egress nick
	 * indicated in the frame header
	 */
	dest = rbr_find_node(rbr, trh->th_egressnick);
	if (unlikely(dest == NULL)) {
		pr_warn_ratelimited("rbr_recv: mulicast  with unknown destination\n");
		goto recv_drop;
	}
	for (idx = 0; idx < dest->rbr_ni->adjcount; idx++) {
		adjnick = RBR_NI_ADJNICK(dest->rbr_ni, idx);
		adj = rbr_find_node(rbr, adjnick);
		if (adj == NULL)
			continue;
		if (memcmp(adj->rbr_ni->adjsnpa, srcaddr, ETH_ALEN) == 0) {
			rbr_node_put(adj);
			break;
		}
		rbr_node_put(adj);
	}

	if (unlikely(idx >= dest->rbr_ni->adjcount)) {
		pr_warn_ratelimited("rbr_recv: multicast unknow mac source\n");
		rbr_node_put(dest);
		goto recv_drop;
	}

	/* Reverse path forwarding check.
	 * Check if the ingress RBridge  that has forwarded
	 * the frame advertised the use of the distribution tree specified
	 * in the egress nick
	 */
	source_node = rbr_find_node(rbr, trh->th_ingressnick);
	if (unlikely(source_node == NULL)) {
		pr_warn_ratelimited("rbr_recv: reverse path forwarding check failed\n");
		rbr_node_put(dest);
		goto recv_drop;
	}
	for (idx = 0; idx < source_node->rbr_ni->dtrootcount; idx++) {
		if (RBR_NI_DTROOTNICK(source_node->rbr_ni, idx) == trh->th_egressnick)
			break;
	}

	if (idx >= source_node->rbr_ni->dtrootcount) {
		/* Allow receipt of forwarded frame with the highest
		 * tree root RBridge as the egress RBridge when the
		 * ingress RBridge has not advertised the use of any
		 * distribution trees.
		 */
		if (source_node->rbr_ni->dtrootcount != 0 ||
		    trh->th_egressnick != dtrNick) {
			rbr_node_put(source_node);
			rbr_node_put(dest);
			goto recv_drop;
		}
	}

	/* Check hop count before doing any forwarding */
	if (unlikely(trill_get_hopcount(trill_flags) == 0)) {
		pr_warn_ratelimited("rbr_recv: multicast hop count limit reached\n");
		rbr_node_put(dest);
		goto recv_drop;
	}
	/* Forward frame using the distribution tree specified by egress nick */
	rbr_node_put(source_node);
	rbr_node_put(dest);

        /* skb2 will be multi forwarded and skb will be locally decaps */
	if (unlikely((skb2 = skb_clone(skb, GFP_ATOMIC)) == NULL)) {
		p->br->dev->stats.tx_dropped++;
		pr_warn_ratelimited("rbr_recv: multicast skb_clone failed\n");
		goto recv_drop;
	}

	if (rbr_multidest_fwd(p, skb2, trh->th_egressnick, trh->th_ingressnick,
			      srcaddr, vid, false))
		goto recv_drop;

	/*
	 * Send de-capsulated frame locally
	 */
	rbr_decaps(p, skb, trhsize, vid);

	return;

recv_drop:
	if (likely(p && p->br))
		p->br->dev->stats.rx_dropped++;
	kfree_skb(skb);
	return;
}

/* handling function hook allow handling
 * a frame upon reception called via
 * br_handle_frame_hook = rbr_handle_frame
 * in  br.c
 * Return NULL if skb is handled
 * note: already called with rcu_read_lock (preempt_disabled)
 */
rx_handler_result_t rbr_handle_frame(struct sk_buff **pskb)
{
	struct net_bridge *br;
	struct net_bridge_port *p;
	uint16_t nick= RBRIDGE_NICKNAME_NONE;
	struct sk_buff *skb = *pskb;
	u16 vid = 0;

	p = br_port_get_rcu(skb->dev);
	if (unlikely(!p))
		goto drop;
	br = p->br;

	/* if trill is not enabled handle by bridge */
	if (br->trill_enabled == BR_NO_TRILL) {
		goto handle_by_bridge;
	} else {
		if (unlikely(skb->pkt_type == PACKET_LOOPBACK))
			return RX_HANDLER_PASS;
		skb = skb_share_check(skb, GFP_ATOMIC);
		if (!skb)
			return RX_HANDLER_CONSUMED;
		if (unlikely(!is_valid_ether_addr(eth_hdr(skb)->h_source))) {
			pr_warn_ratelimited("rbr_handle_frame:invalid src address\n");
			br->dev->stats.rx_dropped++;
			goto drop;
		}
		if (!br_allowed_ingress(p->br, nbp_get_vlan_info(p), skb, &vid))
			goto drop;
		/* don't forward any BPDU */
		if (is_trill_address((const u8*)&eth_hdr(skb)->h_dest)) {
			br_fdb_update(br, p, eth_hdr(skb)->h_source, vid);
			/* BPDU has to be dropped */
			goto drop;
		}

		if (p->trill_flag != TRILL_FLAG_DISABLE) {
			/* check if destination is connected on the same bridge */
			if (is_local_guest_port(p, eth_hdr(skb)->h_dest, vid)) {
				struct net_bridge_fdb_entry *dst;
				dst = __br_fdb_get(br, eth_hdr(skb)->h_dest, vid);
				if (likely (dst)) {
					if (dst->dst->trill_flag != TRILL_FLAG_DISABLE) {
						/* After migration distent vm to local node we need
						* to remove it nickname
						*/
						br_fdb_update(br,
							      p, eth_hdr(skb)->h_source, vid);
						# ifdef CONFIG_TRILL_VNT
						if (unlikely(get_port_vni_id(p) !=
                                                       get_port_vni_id(dst->dst))) {
							br->dev->stats.rx_dropped++;
							goto drop;
						}
						else
						#endif
							br_deliver(dst->dst, skb);
						return RX_HANDLER_CONSUMED;
					}
				}
			}
			/* if packet is from guest port and trill is enabled and dest
			* is not a guest port encaps it
			*/
			nick = get_nick_from_mac(p, eth_hdr(skb)->h_dest, vid);
			/* must update nickname to NONE for guest ports: migration cases */
			br_fdb_update(br, p, eth_hdr(skb)->h_source, vid);
			rbr_encaps_prepare(skb, nick, vid);
			return RX_HANDLER_CONSUMED;
		} else {
			/* packet is not from guest port and trill is enabled */
			if (eth_hdr(skb)->h_proto == __constant_htons(ETH_P_TRILL)) {
				rbr_recv(skb, vid);
				return RX_HANDLER_CONSUMED;
			} else {
				/* packet is destinated to host port */
				if (!compare_ether_addr(p->br->dev->dev_addr,
					eth_hdr(skb)->h_dest)) {
					skb->pkt_type = PACKET_HOST;
					br_handle_frame_finish(skb);
					return RX_HANDLER_CONSUMED;
				} else {
					/* packet is not from trill type drop it */
					br->dev->stats.rx_dropped++;
					goto drop;
				}
			}
		}
	}

drop:
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
handle_by_bridge:
	/* packet is not from trill type return to standard bridge frame handle hook */
	return br_handle_frame(pskb);
}
