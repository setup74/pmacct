/*
 * IS-IS Rout(e)ing protocol - isis_circuit.h
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

#ifndef _ISIS_CIRCUIT_H_
#define _ISIS_CIRCUIT_H_

#define CIRCUIT_MAX 255

struct password
{
  struct password *next;
  int len;
  u_char *pass;
};

struct metric
{
  u_char metric_default;
  u_char metric_error;
  u_char metric_expense;
  u_char metric_delay;
};

struct isis_bcast_info
{
  u_char snpa[ETH_ALEN];	/* SNPA of this circuit */
  char run_dr_elect[2];		/* Should we run dr election ? */
  struct thread *t_run_dr[2];	/* DR election thread */
  struct thread *t_send_lan_hello[2];	/* send LAN IIHs in this thread */
  struct list *adjdb[2];	/* adjacency dbs */
  struct list *lan_neighs[2];	/* list of lx neigh snpa */
  char is_dr[2];		/* Are we level x DR ? */
  u_char l1_desig_is[ISIS_SYS_ID_LEN + 1];	/* level-1 DR */
  u_char l2_desig_is[ISIS_SYS_ID_LEN + 1];	/* level-2 DR */
  struct thread *t_refresh_pseudo_lsp[2];	/* refresh pseudo-node LSPs */
  int pad_hellos;		/* add padding to Hello PDUs ? */
  u_char priority[2];		/* l1/2 IS Priority */
};

struct isis_p2p_info
{
  struct isis_adjacency *neighbor;
  struct thread *t_send_p2p_hello;	/* send P2P IIHs in this thread  */
};

struct interface
{
  char name[SRVBUFLEN];
  unsigned int ifindex;
  unsigned int mtu;
};

struct isis_circuit
{
  int state;
  u_char circuit_id;		/* l1/l2 p2p/bcast CircuitID */
  struct isis_area *area;	/* back pointer to the area */
  struct interface *interface;	/* interface info from z */
  int fd;			/* IS-IS l1/2 socket */
  int sap_length;		/* SAP length for DLPI */
  struct nlpids nlpids;
  /*
   * Threads
   */
  struct thread *t_read;
  struct thread *t_send_csnp[2];
  struct thread *t_send_psnp[2];
  struct list *lsp_queue;	/* LSPs to be txed (both levels) */
  /* there is no real point in two streams, just for programming kicker */
  int (*rx) (struct isis_circuit * circuit, u_char * ssnpa);
  struct stream *rcv_stream;	/* Stream for receiving */
  int (*tx) (struct isis_circuit * circuit, int level);
  struct stream *snd_stream;	/* Stream for sending */
  int idx;			/* idx in S[RM|SN] flags */
#define CIRCUIT_T_BROADCAST  0
#define CIRCUIT_T_P2P        1
#define CIRCUIT_T_STATIC_IN  2
#define CIRCUIT_T_STATIC_OUT 3
#define CIRCUIT_T_DA         4
  int circ_type;		/* type of the physical interface */
  union
  {
    struct isis_bcast_info bc;
    struct isis_p2p_info p2p;
  } u;
  char ext_domain;		/* externalDomain   (boolean) */
  /* 
   * Configurables 
   */
  struct isis_passwd passwd;	/* Circuit rx/tx password */
  long lsp_interval;
  int manual_l2_only;		/* manualL2OnlyMode (boolean) */
  int circuit_is_type;		/* circuit is type == level of circuit
				 * diffrenciated from circuit type (media) */
  u_int32_t hello_interval[2];	/* l1HelloInterval in msecs */
  u_int16_t hello_multiplier[2];	/* l1HelloMultiplier */
  u_int16_t csnp_interval[2];	/* level-1 csnp-interval in seconds */
  u_int16_t psnp_interval[2];	/* level-1 psnp-interval in seconds */
  struct metric metrics[2];	/* l1XxxMetric */
  u_int32_t te_metric[2];
  struct password *c_rx_passwds;	/* circuitReceivePasswords */
  struct password *c_tc_passwd;	/* circuitTransmitPassword */
  int ip_router;		/* Route IP ? */
  struct list *ip_addrs;	/* our IP addresses */
  int ipv6_router;		/* Route IPv6 ? */
  struct list *ipv6_link;	/* our link local IPv6 addresses */
  struct list *ipv6_non_link;	/* our non-link local IPv6 addresses */
  /* 
   * RFC 2973 IS-IS Mesh Groups 
   */
#define MESH_INACTIVE 0
#define MESH_BLOCKED  1
#define MESH_SET      2
  int mesh_enabled;		/* meshGroupEnabled */
  u_int16_t mesh_group;		/* meshGroup */
  u_int16_t upadjcount[2];
  /*
   * Counters as in 10589--11.2.5.9
   */
  u_int32_t adj_state_changes;	/* changesInAdjacencyState */
  u_int32_t init_failures;	/* intialisationFailures */
  u_int32_t ctrl_pdus_rxed;	/* controlPDUsReceived */
  u_int32_t ctrl_pdus_txed;	/* controlPDUsSent */
  u_int32_t desig_changes[2];	/* lanLxDesignatedIntermediateSystemChanges */
  u_int32_t rej_adjacencies;	/* rejectedAdjacencies */
};

struct sockaddr_ll {
  unsigned short int sll_family;
  unsigned short int sll_protocol;
  int sll_ifindex;
  unsigned short int sll_hatype;
  unsigned char sll_pkttype;
  unsigned char sll_halen;
  unsigned char sll_addr[8];
};

extern struct isis_circuit *isis_circuit_new ();
extern void isis_circuit_del (struct isis_circuit *);
extern void isis_circuit_configure (struct isis_circuit *, struct isis_area *);
extern void isis_circuit_up (struct isis_circuit *);
extern void isis_circuit_deconfigure (struct isis_circuit *, struct isis_area *);
extern int isis_circuit_destroy (struct isis_circuit *);
extern void isis_circuit_if_add (struct isis_circuit *, struct interface *);
extern void isis_circuit_if_del (struct isis_circuit *);
extern void circuit_update_nlpids (struct isis_circuit *);
extern void isis_circuit_update_params (struct isis_circuit *, struct interface *);
extern void isis_circuit_down (struct isis_circuit *circuit);

#endif /* _ISIS_CIRCUIT_H_ */
