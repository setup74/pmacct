/*
 * IS-IS Rout(e)ing protocol               - isis_route.c
 * Copyright (C) 2001,2002   Sampo Saaristo
 *                           Tampere University of Technology      
 *                           Institute of Communications Engineering
 *
 *                                         based on ../ospf6d/ospf6_route.[ch]
 *                                         by Yasuhiro Ohara
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public Licenseas published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.

 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "pmacct.h"
#include "isis.h"

#include "linklist.h"
#include "dict.h"
#include "thread.h"
#include "prefix.h"
#include "table.h"
#include "isis_constants.h"
#include "isis_common.h"
#include "isis_circuit.h"
#include "isis_tlv.h"
#include "isis_lsp.h"
#include "isis_pdu.h"
#include "isis_network.h"
#include "isis_misc.h"
#include "isis_constants.h"
#include "isis_adjacency.h"
#include "isis_flags.h"
#include "isisd.h"
#include "isis_csm.h"
#include "isis_route.h"
#include "isis_spf.h"

extern struct isis *isis;
extern struct thread_master *master;

static struct isis_nexthop *
isis_nexthop_create (struct in_addr *ip, unsigned int ifindex)
{
  struct listnode *node;
  struct isis_nexthop *nexthop;

  for (ALL_LIST_ELEMENTS_RO (isis->nexthops, node, nexthop))
    {
      if (nexthop->ifindex != ifindex)
	continue;
      if (ip && memcmp (&nexthop->ip, ip, sizeof (struct in_addr)) != 0)
	continue;

      nexthop->lock++;
      return nexthop;
    }

  nexthop = calloc(1, sizeof (struct isis_nexthop));
  if (!nexthop)
    {
      Log(LOG_ERR, "ERROR ( %s/core/ISIS ): ISIS-Rte: isis_nexthop_create: out of memory!\n", config.name);
    }

  if (nexthop) {
    nexthop->ifindex = ifindex;
    memcpy (&nexthop->ip, ip, sizeof (struct in_addr));
    isis_listnode_add (isis->nexthops, nexthop);
    nexthop->lock++;
  }

  return nexthop;
}

static void
isis_nexthop_delete (struct isis_nexthop *nexthop)
{
  nexthop->lock--;
  if (nexthop->lock == 0)
    {
      isis_listnode_delete (isis->nexthops, nexthop);
      free(nexthop);
    }

  return;
}

static int
nexthoplookup (struct list *nexthops, struct in_addr *ip,
	       unsigned int ifindex)
{
  struct listnode *node;
  struct isis_nexthop *nh;

  for (ALL_LIST_ELEMENTS_RO (nexthops, node, nh))
    {
      if (!(memcmp (ip, &nh->ip, sizeof (struct in_addr))) &&
	  ifindex == nh->ifindex)
	return 1;
    }

  return 0;
}

static struct isis_nexthop6 *
isis_nexthop6_new (struct in6_addr *ip6, unsigned int ifindex)
{
  struct isis_nexthop6 *nexthop6;

  nexthop6 = calloc(1, sizeof (struct isis_nexthop6));
  if (!nexthop6)
    {
      Log(LOG_ERR, "ERROR ( %s/core/ISIS ): ISIS-Rte: isis_nexthop_create6: out of memory!\n", config.name);
    }

  nexthop6->ifindex = ifindex;
  memcpy (&nexthop6->ip6, ip6, sizeof (struct in6_addr));
  nexthop6->lock++;

  return nexthop6;
}

static struct isis_nexthop6 *
isis_nexthop6_create (struct in6_addr *ip6, unsigned int ifindex)
{
  struct listnode *node;
  struct isis_nexthop6 *nexthop6;

  for (ALL_LIST_ELEMENTS_RO (isis->nexthops6, node, nexthop6))
    {
      if (nexthop6->ifindex != ifindex)
	continue;
      if (ip6 && memcmp (&nexthop6->ip6, ip6, sizeof (struct in6_addr)) != 0)
	continue;

      nexthop6->lock++;
      return nexthop6;
    }

  nexthop6 = isis_nexthop6_new (ip6, ifindex);

  return nexthop6;
}

static void
isis_nexthop6_delete (struct isis_nexthop6 *nexthop6)
{

  nexthop6->lock--;
  if (nexthop6->lock == 0)
    {
      isis_listnode_delete (isis->nexthops6, nexthop6);
      free(nexthop6);
    }

  return;
}

static int
nexthop6lookup (struct list *nexthops6, struct in6_addr *ip6,
		unsigned int ifindex)
{
  struct listnode *node;
  struct isis_nexthop6 *nh6;

  for (ALL_LIST_ELEMENTS_RO (nexthops6, node, nh6))
    {
      if (!(memcmp (ip6, &nh6->ip6, sizeof (struct in6_addr))) &&
	  ifindex == nh6->ifindex)
	return 1;
    }

  return 0;
}

static void
adjinfo2nexthop (struct list *nexthops, struct isis_adjacency *adj)
{
  struct isis_nexthop *nh;
  struct listnode *node;
  struct in_addr *ipv4_addr;

  if (adj->ipv4_addrs == NULL)
    return;

  for (ALL_LIST_ELEMENTS_RO (adj->ipv4_addrs, node, ipv4_addr))
    {
      if (!nexthoplookup (nexthops, ipv4_addr, adj->circuit->interface->ifindex))
	{
	  nh = isis_nexthop_create (ipv4_addr, adj->circuit->interface->ifindex);
	  isis_listnode_add (nexthops, nh);
	}
    }
}

static void
adjinfo2nexthop6 (struct list *nexthops6, struct isis_adjacency *adj)
{
  struct listnode *node;
  struct in6_addr *ipv6_addr;
  struct isis_nexthop6 *nh6;

  if (!adj->ipv6_addrs)
    return;

  for (ALL_LIST_ELEMENTS_RO (adj->ipv6_addrs, node, ipv6_addr))
    {
      if (!nexthop6lookup (nexthops6, ipv6_addr,
			   adj->circuit->interface->ifindex))
	{
	  nh6 = isis_nexthop6_create (ipv6_addr,
				      adj->circuit->interface->ifindex);
	  isis_listnode_add (nexthops6, nh6);
	}
    }
}

static struct isis_route_info *
isis_route_info_new (uint32_t cost, uint32_t depth, u_char family,
		     struct list *adjacencies)
{
  struct isis_route_info *rinfo;
  struct isis_adjacency *adj;
  struct listnode *node;

  rinfo = calloc(1, sizeof (struct isis_route_info));
  if (!rinfo)
    {
      Log(LOG_ERR, "ERROR ( %s/core/ISIS ): ISIS-Rte: isis_route_info_new: out of memory!\n", config.name);
      return NULL;
    }

  if (family == AF_INET)
    {
      rinfo->nexthops = isis_list_new ();
      for (ALL_LIST_ELEMENTS_RO (adjacencies, node, adj))
        adjinfo2nexthop (rinfo->nexthops, adj);
    }
  if (family == AF_INET6)
    {
      rinfo->nexthops6 = isis_list_new ();
      for (ALL_LIST_ELEMENTS_RO (adjacencies, node, adj))
        adjinfo2nexthop6 (rinfo->nexthops6, adj);
    }

  rinfo->cost = cost;
  rinfo->depth = depth;

  return rinfo;
}

static void
isis_route_info_delete (struct isis_route_info *route_info)
{
  if (route_info->nexthops)
    {
      route_info->nexthops->del = (void (*)(void *)) isis_nexthop_delete;
      isis_list_delete (route_info->nexthops);
    }

  if (route_info->nexthops6)
    {
      route_info->nexthops6->del = (void (*)(void *)) isis_nexthop6_delete;
      isis_list_delete (route_info->nexthops6);
    }

  free(route_info);
}

static int
isis_route_info_same_attrib (struct isis_route_info *new,
			     struct isis_route_info *old)
{
  if (new->cost != old->cost)
    return 0;
  if (new->depth != old->depth)
    return 0;

  return 1;
}

static int
isis_route_info_same (struct isis_route_info *new,
		      struct isis_route_info *old, u_char family)
{
  struct listnode *node;
  struct isis_nexthop *nexthop;
  struct isis_nexthop6 *nexthop6;
  if (!isis_route_info_same_attrib (new, old))
    return 0;

  if (family == AF_INET)
    {
      for (ALL_LIST_ELEMENTS_RO (new->nexthops, node, nexthop))
        if (nexthoplookup (old->nexthops, &nexthop->ip, nexthop->ifindex) 
              == 0)
          return 0;

      for (ALL_LIST_ELEMENTS_RO (old->nexthops, node, nexthop))
        if (nexthoplookup (new->nexthops, &nexthop->ip, nexthop->ifindex) 
             == 0)
          return 0;
    }
  else if (family == AF_INET6)
    {
      for (ALL_LIST_ELEMENTS_RO (new->nexthops6, node, nexthop6))
        if (nexthop6lookup (old->nexthops6, &nexthop6->ip6,
                            nexthop6->ifindex) == 0)
          return 0;

      for (ALL_LIST_ELEMENTS_RO (old->nexthops6, node, nexthop6))
        if (nexthop6lookup (new->nexthops6, &nexthop6->ip6,
                            nexthop6->ifindex) == 0)
          return 0;
    }

  return 1;
}

static void
isis_nexthops_merge (struct list *new, struct list *old)
{
  struct listnode *node;
  struct isis_nexthop *nexthop;

  for (ALL_LIST_ELEMENTS_RO (new, node, nexthop))
    {
      if (nexthoplookup (old, &nexthop->ip, nexthop->ifindex))
	continue;
      isis_listnode_add (old, nexthop);
      nexthop->lock++;
    }
}

static void
isis_nexthops6_merge (struct list *new, struct list *old)
{
  struct listnode *node;
  struct isis_nexthop6 *nexthop6;

  for (ALL_LIST_ELEMENTS_RO (new, node, nexthop6))
    {
      if (nexthop6lookup (old, &nexthop6->ip6, nexthop6->ifindex))
	continue;
      isis_listnode_add (old, nexthop6);
      nexthop6->lock++;
    }
}

static void
isis_route_info_merge (struct isis_route_info *new,
		       struct isis_route_info *old, u_char family)
{
  if (family == AF_INET)
    isis_nexthops_merge (new->nexthops, old->nexthops);
  else if (family == AF_INET6)
    isis_nexthops6_merge (new->nexthops6, old->nexthops6);

  return;
}

static int
isis_route_info_prefer_new (struct isis_route_info *new,
			    struct isis_route_info *old)
{
  if (!CHECK_FLAG (old->flag, ISIS_ROUTE_FLAG_ACTIVE))
    return 1;

  if (new->cost < old->cost)
    return 1;

  return 0;
}

struct isis_route_info *
isis_route_create (struct isis_prefix *prefix, u_int32_t cost, u_int32_t depth,
		   struct list *adjacencies, struct isis_area *area,
		   int level)
{
  struct route_node *route_node;
  struct isis_route_info *rinfo_new, *rinfo_old, *route_info = NULL;
  u_char buff[BUFSIZ];
  u_char family;

  family = prefix->family;
  /* for debugs */
  isis_prefix2str (prefix, (char *) buff, BUFSIZ);

  rinfo_new = isis_route_info_new (cost, depth, family, adjacencies);
  if (!rinfo_new)
    {
      Log(LOG_ERR, "ERROR ( %s/core/ISIS ): ISIS-Rte (%s): isis_route_create: out of memory!\n",
		config.name, area->area_tag);
      return NULL;
    }

  if (family == AF_INET)
    route_node = route_node_get (area->route_table[level - 1], prefix);
  else if (family == AF_INET6)
    route_node = route_node_get (area->route_table6[level - 1], prefix);
  else {
    if (rinfo_new) free(rinfo_new);
    return NULL;
  }

  rinfo_old = route_node->info;
  if (!rinfo_old)
    {
      if (config.nfacctd_isis_msglog)
        Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route created: %s\n",
		config.name, area->area_tag, area->is_type, buff);
      SET_FLAG (rinfo_new->flag, ISIS_ROUTE_FLAG_ACTIVE);
      route_node->info = rinfo_new;
      return rinfo_new;
    }

  if (config.nfacctd_isis_msglog)
    Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route already exists: %s\n",
		config.name, area->area_tag, area->is_type, buff);

  if (isis_route_info_same (rinfo_new, rinfo_old, family))
    {
      if (config.nfacctd_isis_msglog)
        Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route unchanged: %s\n",
		config.name, area->area_tag, area->is_type, buff);
      isis_route_info_delete (rinfo_new);
      route_info = rinfo_old;
    }
  else if (isis_route_info_same_attrib (rinfo_new, rinfo_old))
    {
      /* merge the nexthop lists */
      if (config.nfacctd_isis_msglog)
        Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route changed (same attribs): %s\n",
		   config.name, area->area_tag, area->is_type, buff);
      isis_route_info_merge (rinfo_new, rinfo_old, family);
      isis_route_info_delete (rinfo_new);
      route_info = rinfo_old;
      UNSET_FLAG (route_info->flag, ISIS_ROUTE_FLAG_ZEBRA_SYNC);
    }
  else
    {
      if (isis_route_info_prefer_new (rinfo_new, rinfo_old))
	{
	  if (config.nfacctd_isis_msglog)
	    Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route changed: %s\n",
			config.name, area->area_tag, area->is_type, buff);
	  isis_route_info_delete (rinfo_old);
	  route_info = rinfo_new;
	}
      else
	{
	  if (config.nfacctd_isis_msglog)
	    Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u) route rejected: %s\n",
			config.name, area->area_tag, area->is_type, buff);
	  isis_route_info_delete (rinfo_new);
	  route_info = rinfo_old;
	}
    }

  SET_FLAG (route_info->flag, ISIS_ROUTE_FLAG_ACTIVE);
  route_node->info = route_info;

  return route_info;
}

static void
isis_route_delete (struct isis_prefix *prefix, struct route_table *table)
{
  struct route_node *rode;
  struct isis_route_info *rinfo;
  char buff[BUFSIZ];

  /* for log */
  isis_prefix2str (prefix, buff, BUFSIZ);


  rode = route_node_get (table, prefix);
  rinfo = rode->info;

  if (rinfo == NULL)
    {
      Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte: tried to delete non-existant route: %s\n", config.name, buff);

      return;
    }

  isis_route_info_delete (rinfo);
  rode->info = NULL;

  return;
}

/* Validating routes in particular table. */
void isis_route_validate_table (struct isis_area *area, struct route_table *table)
{
  struct route_node *rnode, *drnode;
  struct isis_route_info *rinfo;
  u_char buff[BUFSIZ];

  for (rnode = route_top (table); rnode; rnode = route_next (rnode))
    {
      if (rnode->info == NULL)
	continue;
      rinfo = rnode->info;

      if (config.debug)
	{
	  isis_prefix2str (&rnode->p, (char *) buff, BUFSIZ);
	  if (config.nfacctd_isis_msglog)
	    Log(LOG_DEBUG, "DEBUG ( %s/core/ISIS ): ISIS-Rte (tag: %s, level: %u): route validate: %s %s\n",
		      config.name, area->area_tag, area->is_type,
		      (CHECK_FLAG (rinfo->flag, ISIS_ROUTE_FLAG_ACTIVE) ?
		      "active" : "inactive"), buff);
	}

      if (!CHECK_FLAG (rinfo->flag, ISIS_ROUTE_FLAG_ACTIVE))
	{
	  /* Area is either L1 or L2 => we use level route tables directly for
	   * validating => no problems with deleting routes. */
	  if (area->is_type != IS_LEVEL_1_AND_2)
	    {
	      isis_route_delete (&rnode->p, table);
	      continue;
	    }
	  /* If area is L1L2, we work with merge table and therefore must
	   * delete node from level tables as well before deleting route info.
	   * FIXME: Is it performance problem? There has to be the better way.
	   * Like not to deal with it here at all (see the next comment)? */
	  if (rnode->p.family == AF_INET)
	    {
	      drnode = route_node_get (area->route_table[0], &rnode->p);
	      if (drnode->info == rnode->info)
		drnode->info = NULL;
	      drnode = route_node_get (area->route_table[1], &rnode->p);
	      if (drnode->info == rnode->info)
		drnode->info = NULL;
	    }

	  if (rnode->p.family == AF_INET6)
	    {
	      drnode = route_node_get (area->route_table6[0], &rnode->p);
	      if (drnode->info == rnode->info)
		drnode->info = NULL;
	      drnode = route_node_get (area->route_table6[1], &rnode->p);
	      if (drnode->info == rnode->info)
		drnode->info = NULL;
	    }
	      
	  isis_route_delete (&rnode->p, table);
	}
    }
}

/* Function to validate route tables for L1L2 areas. In this case we can't use
 * level route tables directly, we have to merge them at first. L1 routes are
 * preferred over the L2 ones.
 *
 * Merge algorithm is trivial (at least for now). All L1 paths are copied into
 * merge table at first, then L2 paths are added if L1 path for same prefix
 * doesn't already exists there.
 *
 * FIXME: Is it right place to do it at all? Maybe we should push both levels
 * to the RIB with different zebra route types and let RIB handle this? */
void isis_route_validate_merge (struct isis_area *area, int family)
{
  struct route_table *table = NULL;
  struct route_table *merge;
  struct route_node *rnode, *mrnode;

  merge = route_table_init ();

  if (family == AF_INET)
    table = area->route_table[0];
  else if (family == AF_INET6)
    table = area->route_table6[0];

  for (rnode = route_top (table); rnode; rnode = route_next (rnode))
    {
      if (rnode->info == NULL)
        continue;
      mrnode = route_node_get (merge, &rnode->p);
      mrnode->info = rnode->info;
    }

  if (family == AF_INET)
    table = area->route_table[1];
  else if (family == AF_INET6)
    table = area->route_table6[1];

  for (rnode = route_top (table); rnode; rnode = route_next (rnode))
    {
      if (rnode->info == NULL)
        continue;
      mrnode = route_node_get (merge, &rnode->p);
      if (mrnode->info != NULL)
        continue;
      mrnode->info = rnode->info;
    }

  isis_route_validate_table (area, merge);
  route_table_finish (merge);
}

/* Walk through route tables and propagate necessary changes into RIB. In case
 * of L1L2 area, level tables have to be merged at first. */
int
isis_route_validate (struct thread *thread)
{
  struct isis_area *area;

  area = THREAD_ARG (thread);

  if (area->is_type == IS_LEVEL_1)
    { 
      isis_route_validate_table (area, area->route_table[0]);
      goto validate_ipv6;
    }
  if (area->is_type == IS_LEVEL_2)
    {
      isis_route_validate_table (area, area->route_table[1]);
      goto validate_ipv6;
    }

  isis_route_validate_merge (area, AF_INET);

validate_ipv6:
  if (area->is_type == IS_LEVEL_1)
    {
      isis_route_validate_table (area, area->route_table6[0]);
      return ISIS_OK;
    }
  if (area->is_type == IS_LEVEL_2)
    {
      isis_route_validate_table (area, area->route_table6[1]);
      return ISIS_OK;
    }

  isis_route_validate_merge (area, AF_INET6);

  return ISIS_OK;
}
