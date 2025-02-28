/*
 * IS-IS Rout(e)ing protocol - isis_tlv.c
 *                             IS-IS TLV related routines
 *
 * Copyright (C) 2001,2002   Sampo Saaristo
 *                           Tampere University of Technology      
 *                           Institute of Communications Engineering
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
#include "prefix.h"

#include "dict.h"
#include "stream.h"
#include "isis_constants.h"
#include "isis_common.h"
#include "isis_flags.h"
#include "isis_circuit.h"
#include "isis_tlv.h"
#include "isisd.h"
#include "isis_dynhn.h"
#include "isis_misc.h"
#include "isis_pdu.h"
#include "isis_lsp.h"

extern struct isis *isis;

/*
 * Prototypes.
 */
int add_tlv (u_char, u_char, u_char *, struct stream *);

void
free_tlv (void *val)
{
  free(val);

  return;
}

/*
 * Called after parsing of a PDU. There shouldn't be any tlv's left, so this
 * is only a caution to avoid memory leaks
 */
void
free_tlvs (struct tlvs *tlvs)
{
  if (tlvs->area_addrs)
    isis_list_delete (tlvs->area_addrs);
  if (tlvs->is_neighs)
    isis_list_delete (tlvs->is_neighs);
  if (tlvs->te_is_neighs)
    isis_list_delete (tlvs->te_is_neighs);
  if (tlvs->es_neighs)
    isis_list_delete (tlvs->es_neighs);
  if (tlvs->lsp_entries)
    isis_list_delete (tlvs->lsp_entries);
  if (tlvs->lan_neighs)
    isis_list_delete (tlvs->lan_neighs);
  if (tlvs->prefix_neighs)
    isis_list_delete (tlvs->prefix_neighs);
  if (tlvs->ipv4_addrs)
    isis_list_delete (tlvs->ipv4_addrs);
  if (tlvs->ipv4_int_reachs)
    isis_list_delete (tlvs->ipv4_int_reachs);
  if (tlvs->ipv4_ext_reachs)
    isis_list_delete (tlvs->ipv4_ext_reachs);
  if (tlvs->te_ipv4_reachs)
    isis_list_delete (tlvs->te_ipv4_reachs);
  if (tlvs->ipv6_addrs)
    isis_list_delete (tlvs->ipv6_addrs);
  if (tlvs->ipv6_reachs)
    isis_list_delete (tlvs->ipv6_reachs);
  
  return;
}

/*
 * Parses the tlvs found in the variant length part of the PDU.
 * Caller tells with flags in "expected" which TLV's it is interested in.
 */
int
parse_tlvs (char *areatag, u_char * stream, int size, u_int32_t * expected,
	    u_int32_t * found, struct tlvs *tlvs)
{
  u_char type, length;
  struct lan_neigh *lan_nei;
  struct area_addr *area_addr;
  struct is_neigh *is_nei;
  struct te_is_neigh *te_is_nei;
  struct es_neigh *es_nei;
  struct lsp_entry *lsp_entry;
  struct in_addr *ipv4_addr;
  struct ipv4_reachability *ipv4_reach;
  struct te_ipv4_reachability *te_ipv4_reach;
  struct in6_addr *ipv6_addr;
  struct ipv6_reachability *ipv6_reach;
  int prefix_octets;
  int value_len, retval = ISIS_OK;
  u_char *pnt = stream;

  *found = 0;
  memset (tlvs, 0, sizeof (struct tlvs));

  while (pnt < stream + size - 2)
    {
      type = *pnt;
      length = *(pnt + 1);
      pnt += 2;
      value_len = 0;
      if (pnt + length > stream + size)
	{
	  Log(LOG_WARNING, "WARN ( %s/core/ISIS ): ISIS-TLV (%s): TLV (type %d, length %d) exceeds packet boundaries\n",
		config.name, areatag, type, length);
	  retval = ISIS_WARNING;
	  break;
	}
      switch (type)
	{
	case AREA_ADDRESSES:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        Address Length                         | 
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                         Area Address                          | 
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_AREA_ADDRS;
	  if (*expected & TLVFLAG_AREA_ADDRS)
	    {
	      while (length > value_len)
		{
		  area_addr = (struct area_addr *) pnt;
		  value_len += area_addr->addr_len + 1;
		  pnt += area_addr->addr_len + 1;
		  if (!tlvs->area_addrs)
		    tlvs->area_addrs = isis_list_new ();
		  isis_listnode_add (tlvs->area_addrs, area_addr);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case IS_NEIGHBOURS:
	  *found |= TLVFLAG_IS_NEIGHS;
	  if (TLVFLAG_IS_NEIGHS & *expected)
	    {
	      /* +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |                        Virtual Flag                           | 
	       * +-------+-------+-------+-------+-------+-------+-------+-------+
	       */
	      pnt++;
	      value_len++;
	      /* +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |   0   |  I/E  |               Default Metric                  | 
	       * +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |   S   |  I/E  |               Delay Metric                    |
	       * +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |   S   |  I/E  |               Expense Metric                  |
	       * +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |   S   |  I/E  |               Error Metric                    |
	       * +-------+-------+-------+-------+-------+-------+-------+-------+
	       * |                        Neighbour ID                           |
	       * +---------------------------------------------------------------+
	       * :                                                               :
	       */
	      while (length > value_len)
		{
		  is_nei = (struct is_neigh *) pnt;
		  value_len += 4 + ISIS_SYS_ID_LEN + 1;
		  pnt += 4 + ISIS_SYS_ID_LEN + 1;
		  if (!tlvs->is_neighs)
		    tlvs->is_neighs = isis_list_new ();
		  isis_listnode_add (tlvs->is_neighs, is_nei);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case TE_IS_NEIGHBOURS:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        Neighbour ID                           | 7
	   * +---------------------------------------------------------------+
	   * |                        TE Metric                              | 3
	   * +---------------------------------------------------------------+
	   * |                        SubTLVs Length                         | 1
	   * +---------------------------------------------------------------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_TE_IS_NEIGHS;
	  if (TLVFLAG_TE_IS_NEIGHS & *expected)
	    {
	      while (length > value_len)
		{
		  te_is_nei = (struct te_is_neigh *) pnt;
		  value_len += 11;
		  pnt += 11;
		  /* FIXME - subtlvs are handled here, for now we skip */
		  value_len += te_is_nei->sub_tlvs_length;
		  pnt += te_is_nei->sub_tlvs_length;

		  if (!tlvs->te_is_neighs)
		    tlvs->te_is_neighs = isis_list_new ();
		  isis_listnode_add (tlvs->te_is_neighs, te_is_nei);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case ES_NEIGHBOURS:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   0   |  I/E  |               Default Metric                  | 
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Delay Metric                    |
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Expense Metric                  |
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Error Metric                    |
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        Neighbour ID                           |
	   * +---------------------------------------------------------------+
	   * |                        Neighbour ID                           |
	   * +---------------------------------------------------------------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_ES_NEIGHS;
	  if (*expected & TLVFLAG_ES_NEIGHS)
	    {
	      es_nei = (struct es_neigh *) pnt;
	      value_len += 4;
	      pnt += 4;
	      while (length > value_len)
		{
		  /* FIXME FIXME FIXME - add to the list */
		  /*          sys_id->id = pnt; */
		  value_len += ISIS_SYS_ID_LEN;
		  pnt += ISIS_SYS_ID_LEN;
		  /*  if (!es_nei->neigh_ids) es_nei->neigh_ids = sysid; */
		}
	      if (!tlvs->es_neighs)
		tlvs->es_neighs = isis_list_new ();
	      isis_listnode_add (tlvs->es_neighs, es_nei);
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case LAN_NEIGHBOURS:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        LAN Address                            | 
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_LAN_NEIGHS;
	  if (TLVFLAG_LAN_NEIGHS & *expected)
	    {
	      while (length > value_len)
		{
		  lan_nei = (struct lan_neigh *) pnt;
		  if (!tlvs->lan_neighs)
		    tlvs->lan_neighs = isis_list_new ();
		  isis_listnode_add (tlvs->lan_neighs, lan_nei);
		  value_len += ETH_ALEN;
		  pnt += ETH_ALEN;
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case PADDING:
	  pnt += length;
	  break;

	case LSP_ENTRIES:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                     Remaining Lifetime                        | 2
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                         LSP ID                                | id+2
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                   LSP Sequence Number                         | 4
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        Checksum                               | 2
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   */
	  *found |= TLVFLAG_LSP_ENTRIES;
	  if (TLVFLAG_LSP_ENTRIES & *expected)
	    {
	      while (length > value_len)
		{
		  lsp_entry = (struct lsp_entry *) pnt;
		  value_len += 10 + ISIS_SYS_ID_LEN;
		  pnt += 10 + ISIS_SYS_ID_LEN;
		  if (!tlvs->lsp_entries)
		    tlvs->lsp_entries = isis_list_new ();
		  isis_listnode_add (tlvs->lsp_entries, lsp_entry);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case CHECKSUM:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                   16 bit fletcher CHECKSUM                    |
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_CHECKSUM;
	  if (*expected & TLVFLAG_CHECKSUM)
	    {
	      tlvs->checksum = (struct checksum *) pnt;
	    }
	  pnt += length;
	  break;

	case PROTOCOLS_SUPPORTED:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                       NLPID                                   |
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_NLPID;
	  if (*expected & TLVFLAG_NLPID)
	    {
	      tlvs->nlpids = (struct nlpids *) (pnt - 1);
	    }
	  pnt += length;
	  break;

	case IPV4_ADDR:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * +                 IP version 4 address                          + 4
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_IPV4_ADDR;
	  if (*expected & TLVFLAG_IPV4_ADDR)
	    {
	      while (length > value_len)
		{
		  ipv4_addr = (struct in_addr *) pnt;
		  if (!tlvs->ipv4_addrs)
		    tlvs->ipv4_addrs = isis_list_new ();
		  isis_listnode_add (tlvs->ipv4_addrs, ipv4_addr);
		  value_len += 4;
		  pnt += 4;
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case AUTH_INFO:
	  *found |= TLVFLAG_AUTH_INFO;
	  if (*expected & TLVFLAG_AUTH_INFO)
	    {
	      tlvs->auth_info.type = *pnt;
	      tlvs->auth_info.len = length-1;
	      pnt++;
	      memcpy (tlvs->auth_info.passwd, pnt, length - 1);
	      pnt += length - 1;
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case DYNAMIC_HOSTNAME:
	  *found |= TLVFLAG_DYN_HOSTNAME;
	  if (*expected & TLVFLAG_DYN_HOSTNAME)
	    {
	      /* the length is also included in the pointed struct */
	      tlvs->hostname = (struct hostname *) (pnt - 1);
	    }
	  pnt += length;
	  break;

	case TE_ROUTER_ID:
	  /* +---------------------------------------------------------------+
	   * +                         Router ID                             + 4
	   * +---------------------------------------------------------------+
	   */
	  *found |= TLVFLAG_TE_ROUTER_ID;
	  if (*expected & TLVFLAG_TE_ROUTER_ID)
	    tlvs->router_id = (struct te_router_id *) (pnt);
	  pnt += length;
	  break;

	case IPV4_INT_REACHABILITY:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   0   |  I/E  |               Default Metric                  | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Delay Metric                    | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Expense Metric                  | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Error Metric                    | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        ip address                             | 4
	   * +---------------------------------------------------------------+
	   * |                        address mask                           | 4
	   * +---------------------------------------------------------------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_IPV4_INT_REACHABILITY;
	  if (*expected & TLVFLAG_IPV4_INT_REACHABILITY)
	    {
	      while (length > value_len)
		{
		  ipv4_reach = (struct ipv4_reachability *) pnt;
		  if (!tlvs->ipv4_int_reachs)
		    tlvs->ipv4_int_reachs = isis_list_new ();
		  isis_listnode_add (tlvs->ipv4_int_reachs, ipv4_reach);
		  value_len += 12;
		  pnt += 12;
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case IPV4_EXT_REACHABILITY:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   0   |  I/E  |               Default Metric                  | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Delay Metric                    | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Expense Metric                  | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |   S   |  I/E  |               Error Metric                    | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        ip address                             | 4
	   * +---------------------------------------------------------------+
	   * |                        address mask                           | 4
	   * +---------------------------------------------------------------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_IPV4_EXT_REACHABILITY;
	  if (*expected & TLVFLAG_IPV4_EXT_REACHABILITY)
	    {
	      while (length > value_len)
		{
		  ipv4_reach = (struct ipv4_reachability *) pnt;
		  if (!tlvs->ipv4_ext_reachs)
		    tlvs->ipv4_ext_reachs = isis_list_new ();
		  isis_listnode_add (tlvs->ipv4_ext_reachs, ipv4_reach);
		  value_len += 12;
		  pnt += 12;
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case TE_IPV4_REACHABILITY:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        TE Metric                              | 4
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |  U/D  | sTLV? |               Prefix Mask Len                 | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                           Prefix                              | 0-4
	   * +---------------------------------------------------------------+
	   * |                         sub tlvs                              |
	   * +---------------------------------------------------------------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_TE_IPV4_REACHABILITY;
	  if (*expected & TLVFLAG_TE_IPV4_REACHABILITY)
	    {
	      while (length > value_len)
		{
		  te_ipv4_reach = (struct te_ipv4_reachability *) pnt;
		  if (!tlvs->te_ipv4_reachs)
		    tlvs->te_ipv4_reachs = isis_list_new ();
		  isis_listnode_add (tlvs->te_ipv4_reachs, te_ipv4_reach);
		  /* this trickery is permitable since no subtlvs are defined */
		  value_len += 5 + ((te_ipv4_reach->control & 0x3F) ?
				    ((((te_ipv4_reach->control & 0x3F) -
				       1) >> 3) + 1) : 0);
		  pnt += 5 + ((te_ipv4_reach->control & 0x3F) ?
		              ((((te_ipv4_reach->control & 0x3F) - 1) >> 3) + 1) : 0);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case IPV6_ADDR:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * +                 IP version 6 address                          + 16
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * :                                                               :
	   */
	  *found |= TLVFLAG_IPV6_ADDR;
	  if (*expected & TLVFLAG_IPV6_ADDR)
	    {
	      while (length > value_len)
		{
		  ipv6_addr = (struct in6_addr *) pnt;
		  if (!tlvs->ipv6_addrs)
		    tlvs->ipv6_addrs = isis_list_new ();
		  isis_listnode_add (tlvs->ipv6_addrs, ipv6_addr);
		  value_len += 16;
		  pnt += 16;
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case IPV6_REACHABILITY:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                 Default Metric                                | 4 
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                        Control Informantion                   |
	   * +---------------------------------------------------------------+
	   * |                        IPv6 Prefix Length                     |--+
	   * +---------------------------------------------------------------+  |
	   * |                        IPv6 Prefix                            |<-+
	   * +---------------------------------------------------------------+
	   */
	  *found |= TLVFLAG_IPV6_REACHABILITY;
	  if (*expected & TLVFLAG_IPV6_REACHABILITY)
	    {
	      while (length > value_len)
		{
		  ipv6_reach = (struct ipv6_reachability *) pnt;
		  prefix_octets = ((ipv6_reach->prefix_len + 7) / 8);
		  value_len += prefix_octets + 6;
		  pnt += prefix_octets + 6;
		  /* FIXME: sub-tlvs */
		  if (!tlvs->ipv6_reachs)
		    tlvs->ipv6_reachs = isis_list_new ();
		  isis_listnode_add (tlvs->ipv6_reachs, ipv6_reach);
		}
	    }
	  else
	    {
	      pnt += length;
	    }
	  break;

	case WAY3_HELLO:
	  /* +---------------------------------------------------------------+
	   * |                  Adjacency state                              | 1
	   * +---------------------------------------------------------------+
	   * |                  Extended Local Circuit ID                    | 4
	   * +---------------------------------------------------------------+
	   * |                  Neighbor System ID (If known)                | 0-8
	   *                                      (probably 6)
	   * +---------------------------------------------------------------+
	   * |                  Neighbor Local Circuit ID (If known)         | 4
	   * +---------------------------------------------------------------+
	   */
	  *found |= TLVFLAG_3WAY_HELLO;
	  if (*expected & TLVFLAG_3WAY_HELLO)
	    {
	      while (length > value_len)
		{
		  /* FIXME: make this work */
/*           Adjacency State (one octet):
              0 = Up
              1 = Initializing
              2 = Down
            Extended Local Circuit ID (four octets)
            Neighbor System ID if known (zero to eight octets)
            Neighbor Extended Local Circuit ID (four octets, if Neighbor
              System ID is present) */
		  pnt += length;
		}
	    }
	  else
	    {
	      pnt += length;
	    }

	  break;
	case GRACEFUL_RESTART:
	  /* +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |         Reserved                      |  SA   |  RA   |  RR   | 1
	   * +-------+-------+-------+-------+-------+-------+-------+-------+
	   * |                          Remaining Time                       | 2
	   * +---------------------------------------------------------------+
	   * |                Restarting Neighbor ID (If known)              | 0-8
	   * +---------------------------------------------------------------+
	   */
	  *found |= TLVFLAG_GRACEFUL_RESTART;
	  if (*expected & TLVFLAG_GRACEFUL_RESTART)
	    {
	      /* FIXME: make this work */
	    }
	  pnt += length;
	  break;

	default:
	  Log(LOG_WARNING, "WARN ( %s/core/ISIS ): ISIS-TLV (%s): unsupported TLV type %d, length %d\n",
		     	config.name, areatag, type, length);

	  retval = ISIS_WARNING;
	  pnt += length;
	  break;
	}
    }

  return retval;
}

int
add_tlv (u_char tag, u_char len, u_char * value, struct stream *stream)
{

  if (STREAM_SIZE (stream) - stream_get_endp (stream) < (unsigned) len + 2)
    {
      Log(LOG_WARNING, "WARN ( %s/core/ISIS ): No room for TLV of type %d\n", config.name, tag);
      return ISIS_WARNING;
    }

  stream_putc (stream, tag);	/* TAG */
  stream_putc (stream, len);	/* LENGTH */
  stream_put (stream, value, (int) len);	/* VALUE */

  return ISIS_OK;
}

int
tlv_add_area_addrs (struct list *area_addrs, struct stream *stream)
{
  struct listnode *node;
  struct area_addr *area_addr;

  u_char value[255];
  u_char *pos = value;

  for (ALL_LIST_ELEMENTS_RO (area_addrs, node, area_addr))
    {
      if (pos - value + area_addr->addr_len > 255)
	goto err;
      *pos = area_addr->addr_len;
      pos++;
      memcpy (pos, area_addr->area_addr, (int) area_addr->addr_len);
      pos += area_addr->addr_len;
    }

  return add_tlv (AREA_ADDRESSES, pos - value, value, stream);

err:
  Log(LOG_WARNING, "WARN ( %s/core/ISIS ): tlv_add_area_addrs(): TLV longer than 255\n", config.name);
  return ISIS_WARNING;
}

int
tlv_add_is_neighs (struct list *is_neighs, struct stream *stream)
{
  struct listnode *node;
  struct is_neigh *is_neigh;
  u_char value[255];
  u_char *pos = value;
  int retval;

  *pos = 0;			/*is_neigh->virtual; */
  pos++;

  for (ALL_LIST_ELEMENTS_RO (is_neighs, node, is_neigh))
    {
      if (pos - value + IS_NEIGHBOURS_LEN > 255)
	{
	  retval = add_tlv (IS_NEIGHBOURS, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *pos = is_neigh->metrics.metric_default;
      pos++;
      *pos = is_neigh->metrics.metric_delay;
      pos++;
      *pos = is_neigh->metrics.metric_expense;
      pos++;
      *pos = is_neigh->metrics.metric_error;
      pos++;
      memcpy (pos, is_neigh->neigh_id, ISIS_SYS_ID_LEN + 1);
      pos += ISIS_SYS_ID_LEN + 1;
    }

  return add_tlv (IS_NEIGHBOURS, pos - value, value, stream);
}

int
tlv_add_te_is_neighs (struct list *te_is_neighs, struct stream *stream)
{
  struct listnode *node;
  struct te_is_neigh *te_is_neigh;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (te_is_neighs, node, te_is_neigh))
    {
      /* FIXME: This will be wrong if we are going to add TE sub TLVs. */
      if (pos - value + IS_NEIGHBOURS_LEN > 255)
        {
          retval = add_tlv (TE_IS_NEIGHBOURS, pos - value, value, stream);
          if (retval != ISIS_OK)
            return retval;
          pos = value;
        }
      
      memcpy (pos, te_is_neigh->neigh_id, ISIS_SYS_ID_LEN + 1);
      pos += ISIS_SYS_ID_LEN + 1;
      memcpy (pos, te_is_neigh->te_metric, 3);
      pos += 3;
      /* Sub TLVs length. */
      *pos = 0;
      pos++;
    }

  return add_tlv (TE_IS_NEIGHBOURS, pos - value, value, stream);
}

int
tlv_add_lan_neighs (struct list *lan_neighs, struct stream *stream)
{
  struct listnode *node;
  u_char *snpa;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (lan_neighs, node, snpa))
    {
      if (pos - value + ETH_ALEN > 255)
	{
	  retval = add_tlv (LAN_NEIGHBOURS, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      memcpy (pos, snpa, ETH_ALEN);
      pos += ETH_ALEN;
    }

  return add_tlv (LAN_NEIGHBOURS, pos - value, value, stream);
}

int
tlv_add_nlpid (struct nlpids *nlpids, struct stream *stream)
{
  return add_tlv (PROTOCOLS_SUPPORTED, nlpids->count, nlpids->nlpids, stream);
}

int
tlv_add_authinfo (char auth_type, char auth_len, u_char *auth_value,
		  struct stream *stream)
{
  u_char value[255];
  u_char *pos = value;
  *pos++ = ISIS_PASSWD_TYPE_CLEARTXT;
  memcpy (pos, auth_value, auth_len);

  return add_tlv (AUTH_INFO, auth_len + 1, value, stream);
}

int
tlv_add_checksum (struct checksum *checksum, struct stream *stream)
{
  u_char value[255];
  u_char *pos = value;
  return add_tlv (CHECKSUM, pos - value, value, stream);
}

int
tlv_add_ip_addrs (struct list *ip_addrs, struct stream *stream)
{
  struct listnode *node;
  struct prefix_ipv4 *ipv4;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (ip_addrs, node, ipv4))
    {
      if (pos - value + IPV4_MAX_BYTELEN > 255)
	{
	  retval = add_tlv (IPV4_ADDR, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *(u_int32_t *) pos = ipv4->prefix.s_addr;
      pos += IPV4_MAX_BYTELEN;
    }

  return add_tlv (IPV4_ADDR, pos - value, value, stream);
}

/* Used to add TLV containing just one IPv4 address - either IPv4 address TLV
 * (in case of LSP) or TE router ID TLV. */
int
tlv_add_in_addr (struct in_addr *addr, struct stream *stream, u_char tag)
{
  u_char value[255];
  u_char *pos = value;
  
  memcpy (pos, addr, IPV4_MAX_BYTELEN);
  pos += IPV4_MAX_BYTELEN;

  return add_tlv (tag, pos - value, value, stream);
}

int
tlv_add_dynamic_hostname (struct hostname *hostname, struct stream *stream)
{
  return add_tlv (DYNAMIC_HOSTNAME, hostname->namelen, hostname->name,
		  stream);
}

int
tlv_add_lsp_entries (struct list *lsps, struct stream *stream)
{
  struct listnode *node;
  struct isis_lsp *lsp;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (lsps, node, lsp))
    {
      if (pos - value + LSP_ENTRIES_LEN > 255)
	{
	  retval = add_tlv (LSP_ENTRIES, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *((u_int16_t *) pos) = lsp->lsp_header->rem_lifetime;
      pos += 2;
      memcpy (pos, lsp->lsp_header->lsp_id, ISIS_SYS_ID_LEN + 2);
      pos += ISIS_SYS_ID_LEN + 2;
      *((u_int32_t *) pos) = lsp->lsp_header->seq_num;
      pos += 4;
      *((u_int16_t *) pos) = lsp->lsp_header->checksum;
      pos += 2;
    }

  return add_tlv (LSP_ENTRIES, pos - value, value, stream);
}

int
tlv_add_ipv4_reachs (struct list *ipv4_reachs, struct stream *stream)
{
  struct listnode *node;
  struct ipv4_reachability *reach;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (ipv4_reachs, node, reach))
    {
      if (pos - value + IPV4_REACH_LEN > 255)
	{
	  retval =
	    add_tlv (IPV4_INT_REACHABILITY, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *pos = reach->metrics.metric_default;
      pos++;
      *pos = reach->metrics.metric_delay;
      pos++;
      *pos = reach->metrics.metric_expense;
      pos++;
      *pos = reach->metrics.metric_error;
      pos++;
      *(u_int32_t *) pos = reach->prefix.s_addr;
      pos += IPV4_MAX_BYTELEN;
      *(u_int32_t *) pos = reach->mask.s_addr;
      pos += IPV4_MAX_BYTELEN;
    }


  return add_tlv (IPV4_INT_REACHABILITY, pos - value, value, stream);
}

int
tlv_add_te_ipv4_reachs (struct list *te_ipv4_reachs, struct stream *stream)
{
  struct listnode *node;
  struct te_ipv4_reachability *te_reach;
  u_char value[255];
  u_char *pos = value;
  u_char prefix_size;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (te_ipv4_reachs, node, te_reach))
    {
      prefix_size = ((((te_reach->control & 0x3F) - 1) >> 3) + 1);

      if (pos - value + (5 + prefix_size) > 255)
	{
	  retval =
	    add_tlv (IPV4_INT_REACHABILITY, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *(u_int32_t *) pos = te_reach->te_metric;
      pos += 4;
      *pos = te_reach->control;
      pos++;
      memcpy (pos, &te_reach->prefix_start, prefix_size);
      pos += prefix_size;
    }

  return add_tlv (TE_IPV4_REACHABILITY, pos - value, value, stream);
}

int
tlv_add_ipv6_addrs (struct list *ipv6_addrs, struct stream *stream)
{
  struct listnode *node;
  struct prefix_ipv6 *ipv6;
  u_char value[255];
  u_char *pos = value;
  int retval;

  for (ALL_LIST_ELEMENTS_RO (ipv6_addrs, node, ipv6))
    {
      if (pos - value + IPV6_MAX_BYTELEN > 255)
	{
	  retval = add_tlv (IPV6_ADDR, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      memcpy (pos, ipv6->prefix.s6_addr, IPV6_MAX_BYTELEN);
      pos += IPV6_MAX_BYTELEN;
    }

  return add_tlv (IPV6_ADDR, pos - value, value, stream);
}

int
tlv_add_ipv6_reachs (struct list *ipv6_reachs, struct stream *stream)
{
  struct listnode *node;
  struct ipv6_reachability *ip6reach;
  u_char value[255];
  u_char *pos = value;
  int retval, prefix_octets;

  for (ALL_LIST_ELEMENTS_RO (ipv6_reachs, node, ip6reach))
    {
      if (pos - value + IPV6_MAX_BYTELEN + 6 > 255)
	{
	  retval = add_tlv (IPV6_REACHABILITY, pos - value, value, stream);
	  if (retval != ISIS_OK)
	    return retval;
	  pos = value;
	}
      *(uint32_t *) pos = ip6reach->metric;
      pos += 4;
      *pos = ip6reach->control_info;
      pos++;
      prefix_octets = ((ip6reach->prefix_len + 7) / 8);
      *pos = ip6reach->prefix_len;
      pos++;
      memcpy (pos, ip6reach->prefix, prefix_octets);
      pos += prefix_octets;
    }

  return add_tlv (IPV6_REACHABILITY, pos - value, value, stream);
}

int
tlv_add_padding (struct stream *stream)
{
  int fullpads, i, left;

  /*
   * How many times can we add full padding ?
   */
  fullpads = (STREAM_SIZE (stream) - stream_get_endp (stream)) / 257;
  for (i = 0; i < fullpads; i++)
    {
      if (!stream_putc (stream, (u_char) PADDING))	/* TAG */
	goto err;
      if (!stream_putc (stream, (u_char) 255))	/* LENGHT */
	goto err;
      stream_put (stream, NULL, 255);		/* zero padding */
    }

  left = STREAM_SIZE (stream) - stream_get_endp (stream);

  if (left < 2)
    return ISIS_OK;

  if (left == 2)
    {
      stream_putc (stream, PADDING);
      stream_putc (stream, 0);
      return ISIS_OK;
    }

  stream_putc (stream, PADDING);
  stream_putc (stream, left - 2);
  stream_put (stream, NULL, left-2);

  return ISIS_OK;

err:
  Log(LOG_WARNING, "WARN ( %s/core/ISIS ): tlv_add_padding(): no room for tlv\n", config.name);
  return ISIS_WARNING;
}
