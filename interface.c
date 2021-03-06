/* **************************************************************** *
 *                                                                  *
 *  APRX -- 2nd generation APRS iGate and digi with                 *
 *          minimal requirement of esoteric facilities or           *
 *          libraries of any kind beyond UNIX system libc.          *
 *                                                                  *
 * (c) Matti Aarnio - OH2MQK,  2007-2014                            *
 *                                                                  *
 * **************************************************************** */

#include "aprx.h"
#include <sys/socket.h>

/*
 *  The interface subsystem describes all interfaces in one
 *  coherent way, independent of their actual implementation.
 *
 */

/*

<interface>
   serial-device /dev/ttyUSB1 19200 8n1 KISS
   tx-ok         false		# receive only (default)
   callsign      OH2XYZ-R2	# KISS subif 0
   initstring    "...."		# initstring option
   timeout       900            # 900 seconds of no Rx
</interface>

<interface>
   serial-device /dev/ttyUSB2 19200 8n1 KISS
   initstring    "...."
   timeout       900            # 900 seconds of no Rx
   <kiss-subif 0>
      callsign OH2XYZ-2
      tx-ok    true		# This is our transmitter
   </kiss-subif>
   <kiss-subif 1>
      callsign OH2XYZ-R3	# This is receiver
      tx-ok    false		# receive only (default)
   </kiss-subif>
</interface>

<interface>
   tcp-device   172.168.1.1 4001 KISS
   tx-ok         false		# receive only (default)
   callsign      OH2XYZ-R4	# KISS subif 0
   initstring    "...."		# initstring option
   timeout       900            # 900 seconds of no Rx
</interface>

<interface>
   ax25-device OH2XYZ-6		# Works only on Linux systems
   tx-ok       true		# This is also transmitter
</interface>

 */

struct aprx_interface **all_interfaces;
int                     all_interfaces_count;
int			top_interfaces_group;

// Init-code stores this with ifindex = 0.
// This is necessary even for system where igate is removed
struct aprx_interface aprsis_interface = {
	IFTYPE_APRSIS, 0, 0, 0, "APRSIS",
	{'A'<<1,'P'<<1,'R'<<1,'S'<<1,'I'<<1,'S'<<1, 0x60},
	0, NULL,
	0, 0, 0, // subif, txrefcount, tx_ok
        1, 1, 0, // telemeter-to-is, telemeter-to-rf, telemeter-newformat
        0, NULL,
	NULL,
#ifdef ENABLE_AGWPE
	NULL,
#endif
	NULL,
	0, NULL
};

int interface_is_beaconable(const struct aprx_interface *aif)
{
	switch (aif->iftype) {
	case IFTYPE_AX25:
	case IFTYPE_SERIAL:
	case IFTYPE_TCPIP:
	case IFTYPE_NULL:
	case IFTYPE_APRSIS:
	  // case IFTYPE_AGWPE:
	  // These are beaconable.
	  return 1;

	default:
	  break;
	}
	return 0;
}

int interface_is_telemetrable(const struct aprx_interface *aif) {
        // Check if the interface type is really an RF rx and/or tx
	switch (aif->iftype) {
	case IFTYPE_AX25:
	case IFTYPE_SERIAL:
	case IFTYPE_TCPIP:
	  // case IFTYPE_AGWPE:
	  // These are real interfaces, and telemetry sources
	  return 1;

	default:
	  break;
	}
	return 0;
}

#ifndef DISABLE_IGATE
/*
 * A helper for interface_receive_ax25() - analyze 3rd-party packets received
 * via radio.  If data content inside has path saying "TCPIP" or "TCPXX", consider
 * the packet to be indication that fromcall is an IGate.
 */
static void rx_analyze_3rdparty( historydb_t *historydb, struct pbuf_t *pb )
{
	const char *e = pb->data + pb->packet_len - 6;
	const char *p = pb->info_start;
	int from_igate = 0;
	history_cell_t *hist_rx;

	if (!p) return; // Bad packet..
	++p;

	for ( ; p < e; ++p ) {
	  if (*p == ':') break;
	  if (*p == ',') {
	    // The "TCPIP*" or "TCPXX*" will always have preceding ","
	    if (memcmp(",TCPIP*", p, 7) == 0) {
	      from_igate = 1;
	      break;
	    }
	    if (memcmp(",TCPXX*", p, 7) == 0) {
	      from_igate = 1;
	      break;
	    }
	  } // Start with 'T'.
	}

	if (!from_igate) return;  // Not recognized as being sent from another TX-IGATE

	// OK, this packet originated from an TX-IGATE

	// Insert it afresh
	hist_rx = historydb_insert_heard(historydb, pb);
	if (hist_rx != NULL) {
	  // Explicitly mark it as "received from APRSIS"
	  // The packet was received from a TX-IGATE, therefore
	  // the source of that packet is now logged as "from APRSIS".
	  hist_rx->last_heard[0] = pb->t;
	}
}
#endif

static char *interface_default_aliases[] = { "RELAY","WIDE","TRACE" };

static void interface_store(struct aprx_interface *aif)
{
	if (debug)
	  printf("interface_store() aif->callsign = '%s'\n", aif->callsign);

	// Init the interface specific Erlang accounting
	erlang_add(aif->callsign, ERLANG_RX, 0, 0);

	all_interfaces_count += 1;
	all_interfaces = realloc(all_interfaces,
				 sizeof(*all_interfaces) * all_interfaces_count);
	all_interfaces[all_interfaces_count -1] = aif;
	if (aif->ifindex < 0)
	  aif->ifindex = all_interfaces_count -1;
	if (aif->ifgroup < 0) {
          aif->ifgroup = all_interfaces_count; // starting at 1. the 0 is for APRSIS
          /* -- no hard upper limit anymore
          if (aif->ifgroup >= MAX_IF_GROUP)
            aif->ifgroup = MAX_IF_GROUP -1;
          */
        }
	if (top_interfaces_group <= aif->ifgroup)
	  top_interfaces_group = aif->ifgroup +1;
}

struct aprx_interface *find_interface_by_callsign(const char *callsign)
{
	int i;
	for (i = 0; i < all_interfaces_count; ++i) {
	  if ((all_interfaces[i]->callsign != NULL) &&
	      (strcasecmp(callsign, all_interfaces[i]->callsign) == 0)) {
	    return all_interfaces[i];
	  }
	}
	return NULL; // Not found!
}

struct aprx_interface *find_interface_by_index(const int index)
{
	if (index >= all_interfaces_count ||
	    index < 0) {
	  return NULL; // Invalid index value
	} else {
	  return all_interfaces[index];
	}
}

static int config_kiss_subif(struct configfile *cf, struct aprx_interface *aifp, char *param1, char *str, int maxsubif)
{
	struct aprx_interface *aif;
	int   fail = 0;

	char *name;
	int   parlen = 0;

	char *initstring = NULL;
	int   initlength = 0;
	char *callsign   = NULL;
	int   subif      = 0;
        int   tx_ok      = 0;
        int   telemeter_to_is = 1;
        int   telemeter_to_rf = 1;
	int   aliascount = 0;
	char **aliases   = NULL;
	int   ifgroup    = -1;

	const char *p = param1;
	int c;

        if (aifp == NULL || aifp->tty == NULL) {
          printf("%s:%d ERROR: <kiss-subif> on bad type of <interface> entry.\n",
                 cf->name, cf->linenum);
          return 1;

        }

	for ( ; *p; ++p ) {
		c = *p;
		if ('0' <= c && c <= '9') {
			subif = subif * 10 + (c - '0');
		} else if (c == '>') {
		  // all fine..
		  break;
		} else {
		  // FIXME: <KISS-SubIF nnn>  parameter value is bad!
		  printf("%s:%d ERROR: <kiss-subif %s parameter value is bad\n",
			 cf->name, cf->linenum, param1);
		  return 1;
		}
	}
	if (subif >= maxsubif) {
		// FIXME: <KISS-SubIF nnn>  parameter value is bad!
		printf("%s:%d ERROR: <kiss-subif %s parameter value is too large for this KISS variant.\n",
		       cf->name, cf->linenum, param1);
		return 1;
	}

	while (readconfigline(cf) != NULL) {
		if (configline_is_comment(cf))
			continue;	/* Comment line, or empty line */

		// It can be severely indented...
		str = config_SKIPSPACE(cf->buf);

		name = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);
		config_STRLOWER(name);

		param1 = str;
		str = config_SKIPTEXT(str, &parlen);
		str = config_SKIPSPACE(str);

		if (strcmp(name, "</kiss-subif>") == 0) {
		  break; // End of this sub-group
		}

		if (strcmp(name, "callsign") == 0) {

		  if (strcasecmp(param1,"$mycall") == 0)
		    callsign = strdup(mycall);
		  else
		    callsign = strdup(param1);

		  if (!validate_callsign_input(callsign,tx_ok)) {
		    if (tx_ok)
		      printf("%s:%d ERROR: The CALLSIGN parameter on AX25-DEVICE must be of valid AX.25 format! '%s'\n",
			     cf->name, cf->linenum, callsign);
		    else
		      printf("%s:%d ERROR: The CALLSIGN parameter on AX25-DEVICE must be of valid APRSIS format! '%s'\n",
			     cf->name, cf->linenum, callsign);
		    fail = 1;
		    break;
		  }

		  if (find_interface_by_callsign(callsign) != NULL) {
		    // An interface with THIS callsign does exist already!
		    printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
			   cf->name, cf->linenum, callsign);
		    fail = 1;
		    continue;
		  }

		} else if (strcmp(name, "initstring") == 0) {

                  if (initstring == NULL) {
                    initlength = parlen;
                    initstring = malloc(parlen);
                    memcpy(initstring, param1, parlen);
                  } else {
		    printf("%s:%d ERROR: Double-definition of initstring parameter.\n",
			   cf->name, cf->linenum);
		    fail = 1;
                    break;
                  }

		} else if (strcmp(name, "tx-ok") == 0) {
		  if (!config_parse_boolean(param1, &tx_ok)) {
		    printf("%s:%d ERROR: Bad TX-OK parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    fail = 1;
		    break;
		  }

		} else if (strcmp(name, "telem-to-is") == 0) {
		  if (!config_parse_boolean(param1, &telemeter_to_is)) {
		    printf("%s:%d ERROR: Bad TELEM-TO-IS parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    fail = 1;
		    break;
		  }

		} else if (strcmp(name, "telem-to-rf") == 0) {
		  if (!config_parse_boolean(param1, &telemeter_to_rf)) {
		    printf("%s:%d ERROR: Bad TELEM-TO-RF parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    fail = 1;
		    break;
		  }

		} else if (strcmp(name, "alias") == 0) {
		  char *k = strtok(param1, ",");
		  for (; k ; k = strtok(NULL,",")) {
		    ++aliascount;
		    if (debug) printf(" n=%d alias='%s'\n",aliascount,k);
		    aliases = realloc(aliases, sizeof(char*) * aliascount);
		    aliases[aliascount-1] = strdup(k);
		  }

#ifndef DISABLE_IGATE
		} else if (strcmp(name, "igate-group") == 0) {
		  // param1 = integer 1 to N.
		  ifgroup = atol(param1);
		  if (ifgroup < 1) {
		    printf("%s:%d ERROR: interface 'igate-group' parameter value: '%s'  is an integer with minimum value of 1.\n",
			   cf->name, cf->linenum, param1);
		    fail = 1;
		    break;
                    /* -- no hard upper limit anymore
		  } else if (ifgroup >= MAX_IF_GROUP) {
		    printf("%s:%d ERROR: interface 'igate-group' parameter value: '%s'  is an integer with maximum value of %d.\n",
			   cf->name, cf->linenum, param1, MAX_IF_GROUP-1);
		    fail = 1;
		    break;
                    */
		  }
#endif

		} else {
		  printf("%s:%d ERROR: Unrecognized <interface> block keyword: %s\n",
			   cf->name, cf->linenum, name);
		  fail = 1;
		  break;
		}
	}
	if (fail) {
        ERRORMEMFREE:
          if (aliases != NULL) free(aliases);
          if (initstring != NULL) free(initstring);
          return 1; // this leaks memory (but also diagnoses bad input)
        }

	if (callsign == NULL) {
	  // FIXME: Must define at least a callsign!
	  printf("%s:%d ERROR: <kiss-subif ..> MUST define CALLSIGN parameter!\n",
		 cf->name, cf->linenum);
          goto ERRORMEMFREE;
	}

	if (find_interface_by_callsign(callsign) != NULL) {
	  // An interface with THIS callsign does exist already!
	  printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
		 cf->name, cf->linenum, callsign);
          goto ERRORMEMFREE;
	}

        if (debug)
          printf(" Defining <kiss-subif %d>  callsign=%s txok=%s\n", subif, callsign,
                 tx_ok ? "true":"false");


	aif = malloc(sizeof(*aif));
	memcpy(aif, aifp, sizeof(*aif));

	aif->callsign = callsign;
	parse_ax25addr(aif->ax25call, callsign, 0x60);
	aif->subif    = subif;
	aif->tx_ok    = tx_ok;
        aif->telemeter_to_is = telemeter_to_is;
        aif->telemeter_to_rf = telemeter_to_rf;
        // aif->telemeter_newformat = ...
	aif->ifindex  = -1; // system sets automatically at store time
	aif->ifgroup  = ifgroup; // either user sets, or system sets at store time

        aifp->tty->interface  [subif] = aif;
        aifp->tty->ttycallsign[subif] = callsign;
#ifdef PF_AX25	/* PF_AX25 exists -- highly likely a Linux system ! */
        aifp->tty->netax25    [subif] = netax25_open(callsign);
#endif
        if (initstring != NULL) {
          aifp->tty->initlen[subif]    = initlength;
          aifp->tty->initstring[subif] = initstring;
        }

	if (aliascount == 0 || aliases == NULL) {
	  aif->aliascount = 3;
	  aif->aliases    = interface_default_aliases;
	} else {
	  aif->aliascount = aliascount;
	  aif->aliases    = aliases;
	}

	return 0;
}

void interface_init()
{
	interface_store( &aprsis_interface );
}

int interface_config(struct configfile *cf)
{
	struct aprx_interface *aif = calloc(1, sizeof(*aif));

	char  *name, *param1;
	char  *str = cf->buf;
	int    parlen     = 0;
	int    have_fault = 0;
	int    maxsubif   = 16;  // 16 for most KISS modes, 8 for SMACK
	int    defined_subinterface_count = 0;
	int    ifgroup    = -1;

	aif->iftype = IFTYPE_UNSET;
	aif->aliascount = 3;
	aif->aliases    = interface_default_aliases;
	aif->ifindex    = -1; // system sets automatically at store time
	aif->ifgroup    = ifgroup; // either user sets, or system sets at store time
        aif->tx_ok      = 0;
        aif->telemeter_to_is = 1;
        aif->telemeter_to_rf = 1;
        aif->telemeter_newformat = 0;

	while (readconfigline(cf) != NULL) {
		if (configline_is_comment(cf))
			continue;	/* Comment line, or empty line */

		// It can be severely indented...
		str = config_SKIPSPACE(cf->buf);

		name = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);
		config_STRLOWER(name);

		param1 = str;
		str = config_SKIPTEXT(str, &parlen);
		str = config_SKIPSPACE(str);

		if (strcmp(name, "</interface>") == 0) {
		  // End of this interface definition

		  // make the interface...

		  break;
		}
		if (strcmp(name, "<kiss-subif") == 0) {
		  if (config_kiss_subif(cf, aif, param1, str, maxsubif)) {
		    // Bad inputs.. complained already
		    have_fault = 1;
		  }
		  // Always count as defined, even when an error happened!
		  ++defined_subinterface_count;

		  continue;
		}

		// Interface parameters

		if (strcmp(name,"ax25-device") == 0) {
#ifdef PF_AX25		// PF_AX25 exists -- highly likely a Linux system !
		  if (aif->iftype == IFTYPE_UNSET) {
		    aif->iftype = IFTYPE_AX25;
		    // aif->nax25p = NULL;
		  } else {
		    printf("%s:%d ERROR: Only single device specification per interface block!\n",
			   cf->name, cf->linenum);
		    have_fault = 1;
		    continue;
		  }

		  if (strcasecmp(param1,"$mycall") == 0)
		    param1 = strdup(mycall);

		  if (!validate_callsign_input(param1,1)) {
		    printf("%s:%d ERROR: The CALLSIGN parameter on AX25-DEVICE must be of valid AX.25 format! '%s'\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }

		  if (find_interface_by_callsign(param1) != NULL) {
		    // An interface with THIS callsign does exist already!
		    printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }

		  if (debug)
		    printf("%s:%d: AX25-DEVICE '%s' '%s'\n",
			   cf->name, cf->linenum, param1, str);

		  aif->callsign = strdup(param1);
		  parse_ax25addr(aif->ax25call, aif->callsign, 0x60);
		  aif->nax25p = netax25_addrxport(param1, aif);
		  if (aif->nax25p == NULL) {
		    printf("%s:%d ERROR: Failed to open this AX25-DEVICE: '%s'\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }
#else
		  printf("%s:%d ERROR: AX25-DEVICE interfaces are not supported at this system!\n",
			 cf->name, cf->linenum);
		  have_fault = 1;
#endif

		} else if ((strcmp(name,"serial-device") == 0) && (aif->tty == NULL)) {

		  if (aif->iftype == IFTYPE_UNSET) {
		    aif->iftype              = IFTYPE_SERIAL;
		    aif->tty                 = ttyreader_new();
		    aif->tty->ttyname        = strdup(param1);
		    aif->tty->interface[0]   = aif;
		    aif->tty->ttycallsign[0] = mycall;

		    // end processing registers it

		  } else {
		    printf("%s:%d ERROR: Only single device specification per interface block!\n",
			   cf->name, cf->linenum);
		    have_fault = 1;
		    continue;
		  }

		  if (debug)
		    printf(".. new style serial:  '%s' '%s'.. tncid=0\n",
			   aif->tty->ttyname, str);

		  have_fault |= ttyreader_parse_ttyparams(cf, aif->tty, str);

		  switch (aif->tty->linetype) {
		  case LINETYPE_KISSSMACK:
		    maxsubif = 8;  // 16 for most KISS modes, 8 for SMACK
		    break;
		  case LINETYPE_KISSFLEXNET:
		    // ???
		    break;
		  default:
		    break;
		  }

		  // Always count as defined, even when an error happened!
		  ++defined_subinterface_count;

		} else if ((strcmp(name,"tcp-device") == 0) && (aif->tty == NULL)) {
		  int len;
		  char *host, *port;

		  if (aif->iftype == IFTYPE_UNSET) {
		    aif->iftype = IFTYPE_TCPIP;
		    aif->tty = ttyreader_new();
		    aif->tty->interface[0] = aif;
		    aif->tty->ttycallsign[0]  = mycall;

		    // end-step processing registers it

		  } else {
		    printf("%s:%d ERROR: Only single device specification per interface block!\n",
			   cf->name, cf->linenum);
		    have_fault = 1;
		    continue;
		  }

		  host = param1;

		  port = str;
		  str = config_SKIPTEXT(str, NULL);
		  str = config_SKIPSPACE(str);

		  if (debug)
		    printf(".. new style tcp!:  '%s' '%s' '%s'..\n",
			   host, port, str);

		  len = strlen(host) + strlen(port) + 8;

		  aif->tty->ttyname = malloc(len);
		  sprintf((char *) (aif->tty->ttyname), "tcp!%s!%s!", host, port);

		  have_fault |= ttyreader_parse_ttyparams( cf, aif->tty, str );

		  switch (aif->tty->linetype) {
		  case LINETYPE_KISSSMACK:
		    maxsubif = 8;  // 16 for most KISS modes, 8 for SMACK
		    break;
		  case LINETYPE_KISSFLEXNET:
		    // ???
		    break;
		  default:
		    break;
		  }

		  // Always count as defined, even when an error happened!
		  ++defined_subinterface_count;

		} else if (strcmp(name,"null-device") == 0) {
		  if (aif->iftype == IFTYPE_UNSET) {
		    aif->iftype = IFTYPE_NULL;
		    // aif->nax25p = NULL;
		  } else {
		    printf("%s:%d ERROR: Only single device specification per interface block!\n",
			   cf->name, cf->linenum);
		    have_fault = 1;
		    continue;
		  }
		  aif->tx_ok = 1;

		  if (strcasecmp(param1,"$mycall") == 0)
		    param1 = strdup(mycall);

		  if (find_interface_by_callsign(param1) != NULL) {
		    // An interface with THIS callsign does exist already!
		    printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }

                  if (!have_fault) {
		    aif->iftype = IFTYPE_TCPIP;
		    aif->tty = ttyreader_new();
		    aif->tty->interface[0] = aif;
		    aif->tty->ttycallsign[0]  = mycall;
                  }
		  have_fault |= ttyreader_parse_nullparams(cf, aif->tty, str);


		  if (debug)
		    printf("%s:%d: NULL-DEVICE '%s' '%s'\n",
			   cf->name, cf->linenum, param1, str);

		  aif->callsign = strdup(param1);
		  parse_ax25addr(aif->ax25call, aif->callsign, 0x60);


#ifdef ENABLE_AGWPE
		} else if ((strcmp(name,"agwpe-device") == 0) && (aif->tty == NULL)) {

		  // agwpe-device hostname hostport callsign agwpeportnum

		  int len;
		  const char *hostname, *hostport;
		  char *callsign, *agwpeportnum;

		  if (aif->iftype == IFTYPE_UNSET) {
		    aif->iftype = IFTYPE_AGWPE;
		    aif->tty = ttyreader_new();
		    aif->tty->interface[0] = aif;
		    aif->tty->ttycallsign[0]  = mycall;

		    // end-step processing registers it

		  } else {
		    printf("%s:%d ERROR: Only single device specification per interface block!\n",
			   cf->name, cf->linenum);
		    have_fault = 1;
		    continue;
		  }

		  hostname = strdup(param1);
		  hostport = str;
		  str = config_SKIPTEXT(str, NULL);
		  str = config_SKIPSPACE(str);
		  hostport = strdup(hostport);

		  callsign = str;
		  str = config_SKIPTEXT(str, NULL);
		  str = config_SKIPSPACE(str);

		  agwpeportnum = str;
		  str = config_SKIPTEXT(str, NULL);
		  str = config_SKIPSPACE(str);

		  if (debug)
		    printf(".. AGWPE-DEVICE:  '%s' '%s' '%s' '%s' ('%s'...)\n",
			   hostname, hostport, callsign, agwpeportnum, str);

		  len = strlen(hostname) + strlen(hostport) + strlen(agwpeportnum) + 8;
		  aif->tty->ttyname = malloc(len);
		  sprintf((char *) (aif->tty->ttyname), "tcp!%s!%s[%s]",
			  hostname, hostport, agwpeportnum);


		  if (strcasecmp(callsign,"$mycall") == 0)
		    callsign = strdup(mycall);
		  else
		    callsign = strdup(callsign);

		  if (!validate_callsign_input(callsign,1)) {
		    printf("%s:%d ERROR: The CALLSIGN parameter on AGWPE-DEVICE must be of valid AX.25 format! '%s'\n",
			   cf->name, cf->linenum, callsign);
		    have_fault = 1;
		    continue;
		  }

		  if (find_interface_by_callsign(callsign) != NULL) {
		    // An interface with THIS callsign does exist already!
		    printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
			   cf->name, cf->linenum, callsign);
		    have_fault = 1;
		    continue;
		  }

		  aif->callsign = callsign;
		  parse_ax25addr(aif->ax25call, aif->callsign, 0x60);
		  aif->agwpe = agwpe_addport(hostname, hostport, agwpeportnum, aif);
		  if (aif->agwpe == NULL) {
		    printf("%s:%d ERROR: Failed to setup this AGWPE-DEVICE: '%s'\n",
			   cf->name, cf->linenum, callsign);
		    have_fault = 1;
		    continue;
		  }

		  // Always count as defined, even when an error happened!
		  ++defined_subinterface_count;
#endif
		} else if (strcmp(name,"tx-ok") == 0) {

                  int bool;
		  if (!config_parse_boolean(param1, &bool)) {
		    printf("%s:%d ERROR: Bad TX-OK parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }
                  aif->tx_ok = bool;
		  if (bool && aif->callsign) {
		    if (!validate_callsign_input(aif->callsign,bool)) {  // Transmitters REQUIRE valid AX.25 address
		      printf("%s:%d: ERROR: TX-OK 'TRUE' -- BUT PREVIOUSLY SET CALLSIGN IS NOT VALID AX.25 ADDRESS \n",
			     cf->name, cf->linenum);
		      continue;
		    }
		  }

		} else if (strcmp(name, "telem-to-is") == 0) {
                  int bool;
		  if (!config_parse_boolean(param1, &bool)) {
		    printf("%s:%d ERROR: Bad TELEM-TO-IS parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    break;
		  }
                  aif->telemeter_to_is = bool;

		} else if (strcmp(name, "telem-to-rf") == 0) {
                  int bool;
		  if (!config_parse_boolean(param1, &bool)) {
		    printf("%s:%d ERROR: Bad TELEM-TO-RF parameter value -- not a recognized boolean: %s\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    break;
		  }
                  aif->telemeter_to_rf = bool;

		} else if (strcmp(name,"timeout") == 0) {
		  if (config_parse_interval(param1, &(aif->timeout) ) ||
		      (aif->timeout < 0) || (aif->timeout > 14400)) {
		    aif->timeout = 0;
		    printf("%s:%d ERROR: Bad TIMEOUT parameter value: '%s' accepted range: 0s to 4h.\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }
		  if (aif->tty != NULL) {
		    aif->tty->read_timeout = aif->timeout;
		  }

		} else if (strcmp(name, "callsign") == 0) {
		  if (strcasecmp(param1,"$mycall") == 0)
		    param1 = strdup(mycall);

		  if (find_interface_by_callsign(param1) != NULL) {
		    // An interface with THIS callsign does exist already!
		    printf("%s:%d ERROR: Same callsign (%s) exists already on another interface.\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
		  }

		  if (!validate_callsign_input(param1, aif->tx_ok)) {
		    if (aif->tx_ok && aif->iftype != IFTYPE_NULL) {
		      printf("%s:%d ERROR: The CALLSIGN parameter on transmit capable interface must be of valid AX.25 format! '%s'\n",
			     cf->name, cf->linenum, param1);
		      have_fault = 1;
		      continue;
		    }
		  }

		  if (aif->callsign != NULL)
		    free(aif->callsign);
		  aif->callsign = strdup(param1);
		  parse_ax25addr(aif->ax25call, aif->callsign, 0x60);
		  if (aif->tty != NULL)
		    aif->tty->ttycallsign[0] = aif->callsign;

		  if (debug)
		    printf("  callsign= '%s'\n", aif->callsign);

		} else if (strcmp(name, "initstring") == 0) {
		  
		  if (aif->tty != NULL) {
		    int   initlength = parlen;
		    char *initstring = malloc(parlen);
		    memcpy(initstring, param1, parlen);
		    aif->tty->initstring[0] = initstring;
		    aif->tty->initlen[0]    = initlength;
		  }

		} else if (strcmp(name, "alias") == 0) {
		  char *k = strtok(param1, ",");
		  if (aif->aliases == interface_default_aliases) {
		    aif->aliascount = 0;
		    aif->aliases = NULL;
		  }
		  for (; k ; k = strtok(NULL,",")) {
		    aif->aliascount += 1;
		    if (debug) printf(" n=%d alias='%s'\n",aif->aliascount,k);
		    aif->aliases = realloc(aif->aliases, sizeof(char*) * aif->aliascount);
		    aif->aliases[aif->aliascount-1] = strdup(k);
		  }

#ifndef DISABLE_IGATE
		} else if (strcmp(name, "igate-group") == 0) {
		  // param1 = integer 1 to N.
		  ifgroup = atol(param1);
		  if (ifgroup < 1) {
		    printf("%s:%d ERROR: interface 'igate-group' parameter value: '%s'  is an integer with minimum value of 1.\n",
			   cf->name, cf->linenum, param1);
		    have_fault = 1;
		    continue;
                    /* -- no hard upper limit anymore
		  } else if (ifgroup >= MAX_IF_GROUP) {
		    printf("%s:%d ERROR: interface 'igate-group' parameter value: '%s'  is an integer with maximum value of %d.\n",
			   cf->name, cf->linenum, param1, MAX_IF_GROUP-1);
		    have_fault = 1;
		    continue;
                    */
		  }
#endif

		} else {
		  printf("%s:%d ERROR: Unknown <interface> config entry name: '%s'\n",
			 cf->name, cf->linenum, name);
		  have_fault = 1;
		}
	}


	while (!have_fault &&
	       aif->callsign == NULL &&
	       (aif->iftype == IFTYPE_SERIAL || aif->iftype == IFTYPE_TCPIP) &&
	       defined_subinterface_count == 1) {

	        // First check if there already is an interface with $mycall
		// callsign on it..
	
		if (find_interface_by_callsign(mycall) != NULL) {
		  // An interface with $MYCALL callsign does exist already!
		  printf("%s:%d ERROR: The $MYCALL callsign (%s) exists already on another interface.\n",
			 cf->name, cf->linenum, mycall);
		  have_fault = 1;
		  break;
		}

		// Supply a default value
		aif->callsign = strdup(mycall);
		parse_ax25addr(aif->ax25call, aif->callsign, 0x60);
		
#ifdef PF_AX25	// PF_AX25 exists -- highly likely a Linux system !
		// With enough defaults being used, the callsign is defined
		// by global "macro"  mycall,  and never ends up activating
		// the tty -> linux kernel kiss/smack pty  interface.
		// This part does that final step for minimalistic config.
		if (aif->tty != NULL &&
		    aif->tty->netax25[0] == NULL &&
		    aif->tty->ttycallsign[0] != NULL) {
			aif->tty->netax25[0]
			  = netax25_open(aif->tty->ttycallsign[0]);
		}
#endif
		// Done it, leave..
		break;
	}

	if (!have_fault) {
		int i;

		if (aif->tty != NULL) {
		  // Register all tty subinterfaces
                  if (debug) printf(" .. store tty subinterfaces\n");
		  for (i = 0; i < maxsubif; ++i) {
		    if (aif->tty->interface[i] != NULL) {
                      if (debug) printf(" .. store interface[%d] callsign='%s'\n",i, aif->tty->interface[i]->callsign);
		      interface_store(aif->tty->interface[i]);
		    }
		  }
		} else {
		  // Not TTY multiplexed ( = KISS ) interface,
		  // register just the primary.
		  aif->ifgroup = ifgroup; // either user sets, or system sets at store time
		  interface_store(aif);

                  if (debug) printf(" .. store other interface\n");

		}

		if (aif->iftype == IFTYPE_SERIAL)
		  ttyreader_register(aif->tty);

		if (aif->iftype == IFTYPE_TCPIP)
		  ttyreader_register(aif->tty);

	} else {
        	if (aif->callsign) free(aif->callsign);
                if (aif->tty) {
                  if (aif->tty->ttyname) free((void*)(aif->tty->ttyname));
                }
                
                free(aif);
        }

        // coverity[leaked_storage]
	return have_fault;
}


/*
 * Process received AX.25 packet
 *   - from AIF do find all DIGIPEATERS wanting this source.
 *   - If there are none, end processing.
 *   - Parse the received frame for possible latter filters
 *   - Feed the resulting parsed packet to each digipeater
 *
 *
 * Tx-IGate rules:
 *
 // 2) - sending station has not been heard recently
 //      on radio
 // 1) - verify receiving station has been heard
 //      recently on radio
 // 4) - the receiving station has not been heard via
 //      the Internet within a predefined time period.
 //      (Note that _this_ packet is heard from internet,
 //      so one must not confuse this to history..
 //      Nor this siblings that are being created
 //      one for each tx-interface...)
 // 
 //  A station is said to be heard via the Internet if packets
 //  from the station contain TCPIP* or TCPXX* in the header or
 //  if gated (3rd-party) packets are seen on RF gated by the
 //  station and containing TCPIP or TCPXX in the 3rd-party
 //  header (in other words, the station is seen on RF as being
 //  an IGate). 
 *
 * That is, this part of code collects knowledge of RF-wise near-by TX-IGATEs.
 */

void interface_receive_ax25(const struct aprx_interface *aif,
		const char *ifaddress, const int is_aprs, const int ui_pid,
		const uint8_t *axbuf, const int axaddrlen, const int axlen,
		const char    *tnc2buf, const int tnc2addrlen, const int tnc2len)
{
	int i;
	int digi_like_aprs = is_aprs;

	if (aif == NULL) return;         // Not a real interface for digi use
	if (aif->digisourcecount == 0) {
		if (debug>1) printf("interface_receive_ax25() no receivers for source %s\n",aif->callsign);

		if (!is_aprs) return;
		if (debug > 1) printf("  Adding to histroydb anyways...");
		struct digipeater *digi = digipeater_find_by_iface(aif);
		if (digi == NULL) return;
		historydb_t *historydb = digi->historydb;
		struct pbuf_t *pb = pbuf_new(is_aprs, digi_like_aprs,
				tnc2addrlen, tnc2buf, tnc2len,
				axaddrlen, axbuf, axlen);
		if (pb == NULL) return;
		pb->source_if_group = aif->ifgroup;
		parse_aprs(pb, historydb);
		historydb_insert_heard(historydb, pb);
		pbuf_put(pb);
		return; // No receivers for this source
	}

	if (debug) printf("interface_receive_ax25() from %s axlen=%d tnc2len=%d\n",aif->callsign,axlen,tnc2len);


	// AX.25 address length is missing at least a SRCADDR>DESTADDR
	if (axaddrlen < 14) return;     

	// FIXME: match ui_pid to list of UI PIDs that are treated with similar
	//        digipeat rules as is APRS New-N.

	// ui_pid < 0 means that this frame is not an UI frame at all.
	if (ui_pid >= 0)  digi_like_aprs = 1; // FIXME: more precise matching?


	for (i = 0; i < aif->digisourcecount; ++i) {
		struct digipeater_source *digisource = aif->digisources[i];
#ifndef DISABLE_IGATE
		// Transmitter's HistoryDB
		historydb_t *historydb = digisource->parent->historydb;
#endif

		// Allocate pbuf, it is born "gotten" (refcount == 1)
		struct pbuf_t *pb = pbuf_new(is_aprs, digi_like_aprs,
				tnc2addrlen, tnc2buf, tnc2len,
				axaddrlen, axbuf, axlen);
		if (pb == NULL) {
			// Urgh!  Can't do a thing to this!
			// Likely reason: axlen+tnc2len  > 2100 bytes!
			continue;
		}

		pb->source_if_group = aif->ifgroup;

		// If APRS packet, then parse for APRS meaning ...
		if (is_aprs) {
			int rc = parse_aprs(pb,
#ifndef DISABLE_IGATE
					historydb
#else
					NULL
#endif
					); // don't look inside 3rd party
			char *srcif = aif->callsign;
			if (debug)
				printf(".. parse_aprs() rc=%s  type=0x%02x  srcif=%s  tnc2addr='%s'  info_start='%s'\n",
						rc ? "OK":"FAIL", pb->packettype, srcif, pb->data, pb->info_start);

			// If there are no filters, permit all packets
			if (digisource->src_filters != NULL) {
				int filter_discard =
					filter_process(pb,
							digisource->src_filters,
#ifndef DISABLE_IGATE
							historydb // Transmitter HistoryDB
#else
							NULL
#endif
							);
				// filter_discard > 0: accept
				// filter_discard = 0: indifferent (not reject, not accept), tx-igate rules as is.
				// filter_discard < 0: reject
				if (debug)
					printf("source filtering result: %s\n",
							(filter_discard < 0 ? "DISCARD" :
							 (filter_discard > 0 ? "ACCEPT" : "no-match")));

				if (filter_discard <= 0) {
					pbuf_put(pb);
					continue; // allow only explicitly accepted
				}
			}

#ifndef DISABLE_IGATE
			// Find out IGATE callsign (if any), and record it on transmitter's historydb.
			if (pb->packettype & T_THIRDPARTY) {
				rx_analyze_3rdparty( historydb, pb );
			} else {
				// Everything else, feed to history-db
				historydb_insert_heard( historydb, pb );
			}
#endif
		}

		// Feed it to digipeater ...
		digipeater_receive( digisource, pb);

		// .. and finally free up the pbuf (if refcount goes to zero)
		pbuf_put(pb);
	}
}


/*
 * Process AX.25 packet transmit; beacons, digi output, igate output...
 *
 *   - aif:    output interface
 *   - axaddr: ax.25 address
 *   - axdata: payload content, with control and PID bytes prefixing them
 */

void interface_transmit_ax25(const struct aprx_interface *aif, uint8_t *axaddr, const int axaddrlen, const char *axdata, const int axdatalen)
{
	int axlen = axaddrlen + axdatalen;
	uint8_t *axbuf;

	if (debug) {
	  const char *callsign = "";
	  if (aif != NULL) callsign=aif->callsign;
	  printf("interface_transmit_ax25(aif=%p[%s], .., axlen=%d)\n",
		 aif, callsign, axlen);
	}
	if (axlen == 0) return;
	if (aif == NULL) return;


	switch (aif->iftype) {
	case IFTYPE_SERIAL:
	case IFTYPE_TCPIP:
		// If there is linetype error, kisswrite detects it.
		// Make it into single buffer to give to KISS sender
                if (debug>2) {
                  printf("serial_sendto() len=%d,%d: ",axaddrlen,axdatalen);
                  hexdumpfp(stdout, axaddr, axaddrlen, 1);
                  printf(" // ");
                  hexdumpfp(stdout, (uint8_t*)axdata, axdatalen, 0);
                  printf("\n");
                }

		axbuf = alloca(axlen);
		memcpy(axbuf, axaddr, axaddrlen);
		memcpy(axbuf + axaddrlen, axdata, axdatalen);
		kiss_kisswrite(aif->tty, aif->subif, axbuf, axlen);
		break;
#ifdef PF_AX25	/* PF_AX25 exists -- highly likely a Linux system ! */
	case IFTYPE_AX25:
		// The Linux netax25 sender takes same data as this interface
		netax25_sendto( aif->nax25p,
				axaddr, axaddrlen,
				axdata, axdatalen );
		break;
#endif
#ifdef ENABLE_AGWPE
	case IFTYPE_AGWPE:
		agwpe_sendto( aif->agwpe,
			      axaddr, axaddrlen,
			      axdata, axdatalen );
		break;
#endif
	case IFTYPE_NULL:
		// Efficient transmitter :-)
		if (debug>1)
			printf("tx null-device: %s\n", aif->callsign);

                if (debug>2) {
                  printf("null_sendto() len=%d,%d ",axaddrlen,axdatalen);
                  hexdumpfp(stdout, axaddr, axaddrlen, 1);
                  printf(" // ");
                  hexdumpfp(stdout, (uint8_t*)axdata, axdatalen, 0);
                  printf("\n");
                }

		// Account the transmission anyway ;-)
		erlang_add(aif->callsign, ERLANG_TX, axaddrlen+axdatalen + 10, 1);
		break;
	default:
		break;
	}
}

#ifndef DISABLE_IGATE
/*
 * Process received AX.25 packet  -- for APRSIS
 *   - from AIF do find all DIGIPEATERS wanting this source.
 *   - If there are none, end processing.
 *   - Parse the received frame for possible latter filters
 *   - Feed the resulting parsed packet to each digipeater
 *
 * See:  http://www.aprs-is.net/IGateDetails.aspx
 *
 *  Paths
 *
 * IGates should use the 3rd-party format on RF of
 * IGATECALL>APRS,GATEPATH}FROMCALL>TOCALL,TCPIP,IGATECALL*:original packet data
 * where GATEPATH is the path that the gated packet is to follow
 * on RF. This format will allow IGates to prevent gating the packet
 * back to APRS-IS.
 * 
 * q constructs should never appear on RF.
 * The I construct should never appear on RF.
 * Except for within gated packets, TCPIP and TCPXX should not be
 * used on RF.
 *
 * Part of the Tx-IGate logic is here because we use pbuf_t data blocks:
 *
 *  1) The receiving station has been heard recently
 *     within defined range limits, and more recently
 *     than since given interval T1. (Range as digi-hops [N1]
 *     or coordinates, or both.)
 *
 *  2) The sending station has not been heard via RF
 *     within timer interval T2. (Third-party relayed
 *     frames are not analyzed for this.)
 *
 *  4) the receiving station has not been heard via the Internet
 *     within a predefined time period.
 *     A station is said to be heard via the Internet if packets
 *     from the station contain TCPIP* or TCPXX* in the header or
 *     if gated (3rd-party) packets are seen on RF gated by the
 *     station and containing TCPIP or TCPXX in the 3rd-party
 *     header (in other words, the station is seen on RF as being
 *     an IGate). 
 *
 * 5)  Gate all packets to RF based on criteria set by the sysop
 *     (such as callsign, object name, etc.).
 *
 * c)  Drop everything else.
 */

static uint8_t toaprs[7] =    { 'A'<<1,'P'<<1,'R'<<1,'S'<<1,' '<<1,' '<<1,0x60 };

void interface_receive_3rdparty( const struct aprx_interface *aif,
                                 char       **heads,
                                 const int    headscount,
				 const char  *gwtype,
				 const char  *tnc2data,
				 const int    tnc2datalen )
{
	int d; // digipeater index

        const char *fromcall   = heads[0];
        const char *origtocall = heads[1];

	char     tnc2buf1[2800];
	uint8_t  ax25buf1[2800];

	time_t recent_time = tick.tv_sec - 3600; // "recent" = 1 hour
        uint16_t filter_packettype = 0;
        int ax25addrlen1;
        int ax25len1;
        int rc, tnc2addrlen1, tnc2len1;
        uint8_t *a, *b;
        char    *t;
        struct pbuf_t *pb;


	if (debug)
	  printf("interface_receive_3rdparty() aif=%p, aif->digicount=%d\n",
		 aif, aif ? aif->digisourcecount : -1);


	if (aif == NULL) {
	  return;         // Not a real interface for digi use
	}


        // We have to recognize incoming messages targeted to
        // this server.  For this we need to parse the TNC2 frame.
        // 
        // We have a also filter statements to process here,
        // we need to turn incoming APRSIS frame to something
        // that the filter can process:
        
        // Incoming:
        //   EI7IG-1>APRSX,TCPIP*,qAC,T2IRELAND:@262231z5209.97N/00709.65W_238/019g019t049P006h95b10290.wview_5_19_0
        // Filtered:
        //   MYCALL>APRSX,VIA:}EI7IG-1>APRSX:@262231z5209.97N/00709.65W_238/019g019t049P006h95b10290.wview_5_19_0
        

        a = ax25buf1;
        parse_ax25addr( a, tocall, 0x60 );
        a += 7;
        parse_ax25addr( a, fromcall, 0x60 );
        a += 7;
        // No need to add generated VIA address component
        // to this filter input data
        a[-1] |= 0x01; // end-of-address bit
        ax25addrlen1 = a - ax25buf1;
        *a++ = 0x03;
        *a++ = 0xF0;
        if ((sizeof(ax25buf1) - tnc2datalen) <= (a-ax25buf1)) {
          if (debug) printf(" .. data does not fit on ax25buf");
          return;
        }

        memcpy( a, tnc2data, tnc2datalen );
        a += tnc2datalen;
        ax25len1 = (a - ax25buf1);

        t = tnc2buf1;
        t += sprintf(t, "%s>%s:", fromcall, origtocall);
        tnc2addrlen1 = t - tnc2buf1 - 1;
        if ((sizeof(tnc2buf1) - tnc2datalen) <= (t-tnc2buf1)) {
          if (debug) printf(" .. data does not fit on tnc2buf");
          return;
        }
        memcpy(t, tnc2data, tnc2datalen);
        t += tnc2datalen;
        tnc2len1 = (t - tnc2buf1);

        // Allocate temporary pbuf for filter call use
        pb = pbuf_new(1 /*is_aprs*/, 1 /* digi_like_aprs */, 
                      tnc2addrlen1, tnc2buf1, tnc2len1,
                      ax25addrlen1, ax25buf1, ax25len1);
        if (pb == NULL) {
          // Urgh!  Can't do a thing to this!
          // Likely reason: ax25len+tnc2len  > 2100 bytes!
          if (debug) printf("pbuf_new() returned NULL! Discarding!\n");
          return;
        }

        pb->source_if_group = 0; // 3rd-party frames are always from APRSIS


        // This is APRS packet, parse for APRS meaning ...
        rc = parse_aprs(pb, NULL); // look inside 3rd party -- historydb is looked up again below
        if (debug) {
          const char *srcif = aif->callsign ? aif->callsign : "??";
          printf(".. parse_aprs() rc=%s  type=0x%02x srcif=%s tnc2addr='%s'  info_start='%s'\n",
                 rc ? "OK":"FAIL", pb->packettype, srcif, pb->data,
                 pb->info_start);
        }

        filter_packettype = pb->packettype;

        // Check if it is a message destined to myself, and process if so.
        rc = process_message_to_myself(aif, pb);

        // Drop the temporary pbuf..
        pbuf_put(pb);

        if (rc != 0) {
          return; // Processed as message-to-myself
        }

	if (aif->digisourcecount == 0) {
	  return; // No receivers for this source
	}

	// Feed it to digipeaters ...
	for (d = 0; d < aif->digisourcecount; ++d) {
	  struct digipeater_source *digisrc = aif->digisources[d];
	  struct digipeater        *digi    = digisrc->parent;
	  struct aprx_interface    *tx_aif  = digi->transmitter;
#ifndef DISABLE_IGATE
	  historydb_t            *historydb = digi->historydb;
#endif
	  char *srcif;
	  int  discard_this, filter_discard;
          char     tnc2buf[2800];
          uint8_t  ax25buf[2800];
          int ax25addrlen, ax25len;
          int tnc2addrlen, tnc2len;

          // This is APRS packet, parse for APRS meaning ...
          rc = parse_aprs(pb,
#ifndef DISABLE_IGATE
                          historydb // Transmitter HistoryDB
#else
                          NULL
#endif
                          ); // look inside 3rd party -- TODO: but what HISTORYDB ?
          if (debug) {
            const char *srcif = aif->callsign ? aif->callsign : "??";
            printf(".. parse_aprs() rc=%s  type=0x%02x srcif=%s tnc2addr='%s'  info_start='%s'\n",
                   rc ? "OK":"FAIL", pb->packettype, srcif, pb->data,
                   pb->info_start);
          }


	  // Produced 3rd-party packet:
	  //   IGATECALL>APRS,GATEPATH:}FROMCALL>TOCALL,TCPIP,IGATECALL*:original packet data

	  if (debug) printf("## produce 3rd-party AX.25 frames for transmit, and original source one for filtering:\n");
	  // Parse the TNC2 format to AX.25 format
	  // using ax25buf[] storage area.
	  memcpy(ax25buf,    toaprs, 7);           // AX.25 DEST call

	  // FIXME: should this be IGATECALL, not tx_aif->ax25call ??
	  memcpy(ax25buf+7,  tx_aif->ax25call, 7); // AX.25 SRC call

	  a = ax25buf + 2*7;

          if ((filter_packettype & T_MESSAGE) != 0 && digisrc->msg_path != NULL) {
            if (digisrc->msg_path != NULL) {
              memcpy(a, digisrc->msgviapath, 7);    // AX.25 VIA call for a Message
              a += 7;
            }
          } else {
            if (digisrc->via_path != NULL) {
              memcpy(a, digisrc->ax25viapath, 7);    // AX.25 VIA call
              a += 7;
            }
          }

	  *(a-1) |= 0x01;                  // DEST,SRC(,VIA1) - end-of-address bit
	  ax25addrlen = a - ax25buf;

	  if (debug>2) {
	    printf("ax25hdr ");
	    hexdumpfp(stdout, ax25buf, ax25addrlen, 1);
	    printf("\n");
	  }

	  *a++ = 0x03; // UI
	  *a++ = 0xF0; // PID = 0xF0

          b = a; // AX.25 data body

	  a += sprintf((char*)a, "}%s>%s,%s,%s*:",
		       fromcall, origtocall, gwtype, tx_aif->callsign );
	  ax25len = a - ax25buf;
	  if (tnc2datalen + ax25len > sizeof(ax25buf)) {
	    // Urgh...  Can not fit it in :-(
	    if(debug)printf("data does not fit into ax25buf: %d > %d\n",
			    tnc2datalen+ax25len, (int)sizeof(ax25buf));
	    continue;
	  }
	  memcpy(a, tnc2data, tnc2datalen);
	  ax25len += tnc2datalen;
          a       += tnc2datalen;

          if (debug>1) {
            printf("Formatted AX.25: %s>APRS", tx_aif->callsign);
            if ((filter_packettype & T_MESSAGE) != 0 && digisrc->msg_path != NULL) {
              if (digisrc->msg_path != NULL) {
                printf(",%s", digisrc->msg_path);
              }
            } else {
              if (digisrc->via_path != NULL) {
                printf( ",%s", digisrc->via_path);
              }
            }
            printf(":");
            fwrite(b, 1, a-b, stdout);
            printf("\n");
          }

	  // AX.25 packet is built, now build TNC2 version of it
	  t = tnc2buf;

          // NOTE: Building TNC2 form for filter purposes, that is the data
          //       has original source address, and not out interface specific one!
          // 
	  //t += sprintf(t, "%s>%s", tx_aif->callsign, tocall);
	  t += sprintf(t, "%s>%s", fromcall, origtocall);
	  {
		  int i;
		  for (i=2; i<headscount; i++) {
			  t += sprintf(t, ",%s", heads[i]);
		  }
	  }

          /*
          if ((filter_packettype & T_MESSAGE) != 0 && digisrc->msg_path != NULL) {
            if (digisrc->msg_path != NULL) {
              t += sprintf(t, ",%s", digisrc->msg_path);
            }
          } else {
            if (digisrc->via_path != NULL) {
              t += sprintf(t, ",%s", digisrc->via_path);
            }
          }
          */
	  if (debug>1)printf(" filter tnc2addr = %s\n", tnc2buf);

	  tnc2addrlen = t - tnc2buf;
	  *t++ = ':';
	  t += sprintf(t, "}%s>%s,%s,%s*:",
		       fromcall, origtocall, gwtype, tx_aif->callsign );
	  if (tnc2datalen + (t-tnc2buf) +4 > sizeof(tnc2buf)) {
	    // Urgh...  Can not fit it in :-(
	    if(debug)printf("data does not fit into tnc2buf: %d > %d\n",
			    (int)(tnc2datalen+(t-tnc2buf)+4),
			    (int)sizeof(tnc2buf));
	    continue;
	  }
	  memcpy(t, tnc2data, tnc2datalen);
	  t += tnc2datalen;
	  tnc2len = (t - tnc2buf);

	  // Allocate pbuf, it is born "gotten" (refcount == 1)
	  pb = pbuf_new(1 /*is_aprs*/, 1 /* digi_like_aprs */,
                        tnc2addrlen, tnc2buf, tnc2len,
                        ax25addrlen, ax25buf, ax25len);
	  if (pb == NULL) {
	    // Urgh!  Can't do a thing to this!
	    // Likely reason: ax25len+tnc2len  > 2100 bytes!
	    if (debug) printf("pbuf_new() returned NULL! Discarding!\n");
	    continue;
	  }

	  pb->source_if_group = 0; // 3rd-party frames are always from APRSIS
	  srcif = aif->callsign ? aif->callsign : "??";

	  // This is APRS packet, parse for APRS meaning ...
	  rc = parse_aprs(pb, historydb); // look inside 3rd party
	  if (debug)
	    printf(".. parse_aprs() rc=%s  type=0x%02x srcif=%s tnc2addr='%s'  info_start='%s'\n",
		   rc ? "OK":"FAIL", pb->packettype, srcif, pb->data,
		   pb->info_start);

          // 1) - verify receiving station has been heard
          //      recently on radio
          // 2) - sending station has not been heard recently
          //      on radio
          // 4) - the receiving station has not been heard via
          //      the Internet within a predefined time period.
          //      (Note that _this_ packet is heard from internet,
          //      so one must not confuse this to history..
          //      Nor this siblings that are being created
          //      one for each tx-interface...)
	  // 
	  //  A station is said to be heard via the Internet if packets
	  //  from the station contain TCPIP* or TCPXX* in the header or
	  //  if gated (3rd-party) packets are seen on RF gated by the
	  //  station and containing TCPIP or TCPXX in the 3rd-party
	  //  header (in other words, the station is seen on RF as being
	  //  an IGate). 


	  // Message Tx-IGate rules..
	  discard_this = 0;

	  if (pb->dstname == NULL) {
	    // Sanity -- not a message..
	    discard_this = 1;
	  }
	  if (filter_packettype == 0)
	    filter_packettype = pb->packettype;
	  if ((filter_packettype & T_MESSAGE) == 0) {
	    // Not a message packet
	    discard_this = 1;
	  }
	  if ((filter_packettype & (T_NWS)) != 0) {
	    // Not a weather alert packet
	    discard_this = 1;
	  }

	  // Accept/Reject the packet by digipeater rx filter?
	  filter_discard = 0;
	  if (digisrc->src_filters == NULL) {
	    // No filters defined, default Tx-iGate rules apply
	  } else {

	    if (debug) printf("## process source filter\n");

	    {
	      // Stores position, and message references
	      void *v = historydb_insert_heard( historydb, pb );
	      if (debug) printf("historydb_insert_heard(APRSIS) v=%p\n", v);
	    }

	    filter_discard = filter_process(pb, digisrc->src_filters, historydb);

	    if (debug) printf("filter says: %d (%s)\n", filter_discard, (filter_discard > 0 ? "accept" : (filter_discard == 0 ? "indifferent" : "reject")));

            // filter_discard > 0: accept
            // filter_discard = 0: indifferent (not reject, not accept), tx-igate rules as is.
            // filter_discard < 0: reject

            // Manual filter says: Reject!
	    if (filter_discard < 0) {
	      if (debug) printf("REJECTED!\n");
              discard_this = 1;
	    }
            // Manual filter says: Accept!
            if (discard_this && filter_discard > 0) {
              if (debug) printf("filters say: send!\n");
	    discard_this = 0;
            }
	  }


	  if (!discard_this && pb->dstname != NULL) {
	    // 1) - verify receiving station has been heard
	    //      recently on radio
	    char recipient[10];
	    history_cell_t *hist_rx;
            int i = 0;
            while ( i < 9 && pb->dstname[i] != 0 && pb->dstname[i] != ' ' ) {
              recipient[i] = pb->dstname[i];
              ++i;
            }
            recipient[i] = 0;

            pb->dstname_len = strlen(recipient);

	    // FIXME?  Should test all SSIDs of this target callsign,
	    //         not just this one target,
	    //         if this is a T_MESSAGE!  (strange BoB rules...)

	    hist_rx = historydb_lookup(historydb, recipient, strlen(recipient));
	    if (hist_rx == NULL) {
	      if (debug) printf("No history entry for receiving call: '%s'  DISCARDING.\n", recipient);
	      discard_this = 1;
	    }
	    // See that it has 'heard on radio' flag on this tx interface
	    if (hist_rx != NULL && discard_this == 0) {
	      if (timecmp(hist_rx->last_heard[tx_aif->ifgroup], recent_time) >= 0) {
		// Heard recently enough
		discard_this = 0;
		if (debug) printf("History entry for receiving call '%s' from RADIO is recent enough.  KEEPING.\n", recipient);
	      }
	    }

	    // FIXME: Check that recipient is in our service area
	    //        a) coordinate is "near by"
	    //        b) last known hop-count is low enough
	    //           (FIXME: RF hop-count recording infra needed!)

	    // 4) the receiving station has not been heard via the internet
	    if (hist_rx != NULL && timecmp(hist_rx->last_heard[0], recent_time) > 0) {
	      // "is heard recently via internet"
	      discard_this = 1;
	      if (debug) printf("History entry for sending call '%s' from APRSIS is too new.  DISCARDING.\n", fromcall);
	    }
	  }

	  if (!discard_this) {
	    history_cell_t *hist_tx = historydb_lookup(historydb, fromcall, strlen(fromcall));
	    // If no history entry for this tx callsign,
	    // then rules 2 and 4 permit tx-igate
	    if (hist_tx != NULL) {
	      // There is a history entry for this tx callsign, check rules 2+4
	      // 2) Sending station has not been heard recently on radio (this target)
	      if (timecmp(hist_tx->last_heard[tx_aif->ifgroup], recent_time) > 0) {
		// "is heard recently"
		discard_this = 1;
		if (debug) printf("History entry for sending call '%s' from RADIO is too new.  DISCARDING.\n", fromcall);
	      }
	    }
	  }

	  {
	    // Stores position, and message references
	    void *v = historydb_insert_heard( historydb, pb );
	    if (debug) printf("historydb_insert_heard(APRSIS) v=%p\n",v);
	  }

	  if (filter_discard > 0 || (filter_discard == 0 && !discard_this)) {
	    // Not discarding - approved for transmission

	    if ((filter_packettype & T_POSITION) == 0) {
	      // TODO: For position-less packets send at first a position packet
	      //       for same source call sign -- if available.
	      
	    }

	    if (debug) printf("Send to digipeater\n");
	    digipeater_receive( digisrc, pb);
	  } else {
	    if (debug) printf("DISCARDED! (filter_discard=%d, discard_this=%d)\n",filter_discard, discard_this);
	  }

	  // .. and finally free up the pbuf (if refcount goes to 0)
	  pbuf_put(pb);
	}
}

/*
 * See if this is a message that is destined to myself
 */

#define DSTNAMELEN 16  /* 8+1+2+1 = 12, use 16 for stack align */

static int dstname_is_myself(const struct pbuf_t*const pb, char *dstname, const struct aprx_interface**aifp)
{
	struct aprx_interface *aif;

	// Copy message destination, if available.
        *dstname = 0; // always clear first..
        if (pb->dstname != NULL) {
          strncpy(dstname, pb->dstname, DSTNAMELEN-1);
          dstname[DSTNAMELEN-1] = 0;
        }

        if (strcmp(dstname, mycall) == 0) {
          // To MYCALL account
          return 1;
        }
        if (aprsis_loginid != NULL && strcmp(dstname, aprsis_loginid) == 0) {
          // To APRSIS login account
          return 1;
        }

        // Maybe one of my transmitters?
        aif = find_interface_by_callsign(dstname);
        if (aif != NULL && aif->tx_ok) {
          // To one of my transmitter interfaces
          *aifp = aif;
          return 1;
        }
        // None of my identities
        return 0;
}

/*
 * Ack the message 
 */
static void ack_message(const struct aprx_interface *const srcif, const struct aprx_interface *const aif, const struct pbuf_t*const pb, const struct aprs_message_t*const am, const char*const dstname)
{
	// ACK message to APRSIS is simple(ish), routing it is another thing..
	if (srcif == &aprsis_interface) {
          char destbuf[50];
          int destlen = sprintf(destbuf, "%s>APRS,TCPIP*", dstname);
          char txt[50];
          int txtlen;
          char *t = txt;
          const char *s = pb->srcname;
          int i;
          *t++ = ':';
          for (i = 0; i < 9 && i < pb->srcname_len; ++i) {
            *t++ = *s++;
          }
          for ( ; i < 9 ; ++i) {
            *t++ = ' ';
          }
          *t++ = ':';
          *t++ = 'a';
          *t++ = 'c';
          *t++ = 'k';
          for (i = 0, s = am->msgid; i < am->msgid_len; ++i) {
            *t++ = *s++;
          }
          txtlen = t - txt;

          aprsis_queue(destbuf, destlen,
                       qTYPE_LOCALGEN,
                       aprsis_login, txt, txtlen);

          rflog2("APRSIS", 't', 0, destbuf, txt);
          return;
        }
        // TODO: ACK things sent via radio interfaces?
}

/*
 * A message is destined to myself, lets look closer..
 * Return non-zero if it was recognized as targeted to this node.
 */
int process_message_to_myself(const struct aprx_interface*const srcif, const struct pbuf_t*const pb)
{
	struct aprs_message_t am;
        int rc;
        const struct aprx_interface*aif = srcif;
	char dstname[DSTNAMELEN];

        if ((pb->packettype & T_MESSAGE) == 0) {
          return 0; // Not a message!
        }

        if (!dstname_is_myself(pb, dstname, &aif)) {
          // Not destined to me
          // This will also reject bulletins, which one is not supposed to ACK anyway..
          return 0;
        }

        rc = parse_aprs_message(pb, &am);
        if (rc != 0) {
          // Not acceptable parse result
          return 0;
        }

        // Whatever message, syslog it.
        syslog(LOG_INFO, "%.*s", pb->packet_len, pb->data);

        if (am.is_rej || am.is_ack) {
          // A REJect or ACKnowledge received, drop.
          return 1;
        }

        // If there is msgid in the message -> I need to ACK it.
        if (am.msgid != NULL) {
          ack_message(srcif, aif, pb, &am, dstname);
        }

        // TODO: Process the message ?

        return 1;
}
#endif

/*
 * Process transmit of APRS beacons
 *
 * Note:  txbuf  starts if AX.25 Control+PID bytes!
 */

int interface_transmit_beacon(const struct aprx_interface *aif, const char *src, const char *dest, const char *via, const char *txbuf, const int txlen)
{
	uint8_t ax25addr[90];
	int     ax25addrlen;
	int	have_fault = 0;
	int	viaindex   = 1; // First via field will be index 2
	char    axaddrbuf[128];
	char    *a = axaddrbuf;
	dupecheck_t *dupechecker;
        int     axlen;

	if (debug)
	  printf("interface_transmit_beacon() aif=%p, aif->txok=%d aif->callsign='%s'\n",
		 aif, aif && aif->tx_ok ? 1 : 0, aif ? aif->callsign : "<nil>");

	if (aif == NULL)    return 0;
	if (!aif->tx_ok) return 0; // Sorry, no Tx

	dupechecker = digipeater_find_dupecheck(aif);

	// _FOR_VALGRIND_  -- and just in case for normal use
	memset(ax25addr, 0, sizeof(ax25addr));
	memset(axaddrbuf, 0, sizeof(axaddrbuf));
	
	if (parse_ax25addr(ax25addr +  7, src,  0x60)) {
	  if (debug) printf("parse_ax25addr('%s') failed. [1]\n", src);
	  return -1;
	}
	if (parse_ax25addr(ax25addr +  0, dest, 0x60)) {
	  if (debug) printf("parse_ax25addr('%s') failed. [2]\n", dest);
	  return -1;
	}
	ax25addrlen = 14; // Initial Src+Dest without any Via.

	a += sprintf(axaddrbuf, "%s>%s", src, dest);
	*a = 0;
        axlen = a - axaddrbuf;

	if (via != NULL) {
	  char viafield[12];
          int  vialen = strlen(via);
	  const char *s, *p = via;
	  const char *ve = via + vialen;

	  *a++ = ',';
          axlen = a - axaddrbuf;
          if (vialen > (sizeof(axaddrbuf)-axlen-3))
            vialen = (sizeof(axaddrbuf)-axlen-3);
          if (vialen > 0) {
            memcpy(a, via, vialen);
            a += vialen;
          }
          *a = 0;
          axlen = a - axaddrbuf;

	  while (p < ve) {
	    int len;
	    
	    for (s = p; s < ve; ++s) {
	      if (*s == ',') {
		break;
	      }
	    }
	    // [p..s] is now one VIA field.
	    if (s == p) {  // BAD!
	      have_fault = 1;
	      if (debug>1) printf(" S==P ");
	      break;
	    }
	    ++viaindex;
	    if (viaindex >= 10) {
	      if (debug) printf("too many via-fields: '%s'\n", via);
	      return -1; // Too many VIA fields
	    }

	    len = s - p;
	    if (len >= sizeof(viafield)) len = sizeof(viafield)-1;
	    memcpy(viafield, p, len);
	    viafield[len] = 0;
	    if (*s == ',') ++s;
	    p = s;
	    // VIA-field picked up, now parse it..

	    if (parse_ax25addr(ax25addr + viaindex * 7, viafield, 0x60)) {
	      // Error on VIA field value
	      if (debug) printf("parse_ax25addr('%s') failed. [3]\n", viafield);
	      return -1;
	    }
	    ax25addrlen += 7;
	  }
	}

	if (have_fault) {
	  if (debug) {
	    printf("observed a fault in inputs of interface_transmit_beacon()\n");
	  }
	  return 1;
	}

	ax25addr[ax25addrlen-1] |= 0x01; // set address field end bit


	// Feed to dupe-filter (transmitter specific)
	// this means we have already seen it, and when 
	// it comes back from somewhere, we do not digipeat
	// it ourselves.

	if (dupechecker != NULL)
	  dupecheck_aprs( dupechecker,
			  axaddrbuf, strlen(axaddrbuf),
			  txbuf+2, txlen-2  ); // ignore Ctrl+PID

	// Transmit it to actual radio interface

	interface_transmit_ax25( aif,
				 ax25addr, ax25addrlen,
				 txbuf, txlen);


	if (rflogfile) {
	  char    *axbuf;

	  axbuf = alloca(axlen+txlen+3);
          memcpy( axbuf, axaddrbuf, axlen );
	  a = axbuf + axlen;
	  *a++ = ':';
	  memcpy(a, txbuf+2, txlen-2); // forget control+pid bytes..
	  a += txlen -2;   // final assembled message end pointer

	  rflog(aif->callsign, 'T', 0, axbuf, a - axbuf); // beacon
	}

	return 0;
}
