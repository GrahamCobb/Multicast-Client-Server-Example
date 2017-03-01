/*
  msock.c - multicast socket creation routines

  (C) 2016 Christian Beier <dontmind@sdf.org>

*/


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "msock.h"

static void DieWithError(char* errorMessage)
{
  fprintf(stderr, "%s\n", errorMessage);
  exit(EXIT_FAILURE);
}


SOCKET mcast_send_socket(char* multicastIP, char* multicastPort,  int multicastTTL, struct addrinfo **multicastAddr) {

    SOCKET sock;
    struct addrinfo hints = { 0 };    /* Hints for name lookup */
    
    
#ifdef WIN32
    WSADATA trash;
    if(WSAStartup(MAKEWORD(2,0),&trash)!=0)
	DieWithError("Couldn't init Windows Sockets\n");
#endif

    
    /*
      Resolve destination address for multicast datagrams 
    */
    hints.ai_family   = PF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST;
    int status;
    if ((status = getaddrinfo(multicastIP, multicastPort, &hints, multicastAddr)) != 0 )
	{
	    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
	    return -1;
	}

   

    /* 
       Create socket for sending multicast datagrams 
    */
    if ( (sock = socket((*multicastAddr)->ai_family, (*multicastAddr)->ai_socktype, 0)) < 0 ) {
	perror("socket() failed");
	freeaddrinfo(*multicastAddr);
	return -1;
    }

    /* 
       Set TTL of multicast packet 
    */
    if ( setsockopt(sock,
		    (*multicastAddr)->ai_family == PF_INET6 ? IPPROTO_IPV6        : IPPROTO_IP,
		    (*multicastAddr)->ai_family == PF_INET6 ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL,
		    (char*) &multicastTTL, sizeof(multicastTTL)) != 0 ) {
	perror("setsockopt() failed");
	freeaddrinfo(*multicastAddr);
	return -1;
    }
    
    
    /* 
       set the sending interface 
    */
    if((*multicastAddr)->ai_family == PF_INET) {
	in_addr_t iface = INADDR_ANY; /* well, yeah, any */
	if(setsockopt (sock, 
		       IPPROTO_IP,
		       IP_MULTICAST_IF,
		       (char*)&iface, sizeof(iface)) != 0) { 
	    perror("interface setsockopt() sending interface");
	    freeaddrinfo(*multicastAddr);
	    return -1;
	}

    }
    if((*multicastAddr)->ai_family == PF_INET6) {
	unsigned int ifindex = 0; /* 0 means 'default interface'*/
	if(setsockopt (sock, 
		       IPPROTO_IPV6,
		       IPV6_MULTICAST_IF,
		       (char*)&ifindex, sizeof(ifindex)) != 0) { 
	    perror("interface setsockopt() sending interface");
	    freeaddrinfo(*multicastAddr);
	    return -1;
	}   
	 
    }

     
    return sock;

}

/* Send message on all interfaces */
int mcast_sendto_all(int sockfd, const void *buf, size_t len, int flags,
                      const struct sockaddr *dest_addr, socklen_t addrlen) {

    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) != 0) {
	perror("getifaddrs() failed");
	return -1;
    }

    /* Loop through all multicast-capable interfaces */
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
	if (ifa->ifa_addr == NULL) continue;
	if (ifa->ifa_addr->sa_family != dest_addr->sa_family) continue;
	if ((ifa->ifa_flags & IFF_MULTICAST) == 0) continue;

	/* Select this interface */
	printf("Sending to interface %s\n", ifa->ifa_name);
	if (ifa->ifa_addr->sa_family == AF_INET) {
	    if (setsockopt (sockfd, IPPROTO_IP, IP_MULTICAST_IF,
			    ifa->ifa_addr, sizeof(struct sockaddr_in)) != 0) {
		perror("setsockopt failed selecting IPv4 interface");
		freeifaddrs(ifaddr);
		return -1;
	    }
	}
	if (ifa->ifa_addr->sa_family == AF_INET6) {
	    if (setsockopt (sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
			    ifa->ifa_addr, sizeof(struct sockaddr_in6)) != 0) {
		perror("setsockopt() failed selecting IPv6 interface");
		freeifaddrs(ifaddr);
		return -1;
	    }
	}

	if (sendto(sockfd, buf, len, flags, dest_addr, addrlen) != len) {
	    perror("sendto() sent a different number of bytes than expected");
	    freeifaddrs(ifaddr);
	    return -1;
	}
    }

    freeifaddrs(ifaddr);
    return 0;
}


SOCKET mcast_recv_socket(char* multicastIP, char* multicastPort, int multicastRecvBufSize) {

    SOCKET sock;
    struct addrinfo   hints  = { 0 };    /* Hints for name lookup */
    struct addrinfo*  localAddr = 0;         /* Local address to bind to */
    struct addrinfo*  multicastAddr = 0;     /* Multicast Address */
    int yes=1;
    struct ifaddrs *ifaddr = 0;
    struct ifaddrs *ifa;
    unsigned char if_index_used[MAX_IF_INDEX+1];
  
#ifdef WIN32
    WSADATA trash;
    if(WSAStartup(MAKEWORD(2,0),&trash)!=0)
	DieWithError("Couldn't init Windows Sockets\n");
#endif

    memset(if_index_used, 0, sizeof(if_index_used[0]) * (MAX_IF_INDEX+1));
    
    /* Resolve the multicast group address */
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags  = AI_NUMERICHOST;
    int status;
    if ((status = getaddrinfo(multicastIP, NULL, &hints, &multicastAddr)) != 0) {
	    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
	    goto error;
    }
    
   
    /* 
       Get a local address with the same family (IPv4 or IPv6) as our multicast group
       This is for receiving on a certain port.
    */
    hints.ai_family   = multicastAddr->ai_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE; /* Return an address we can bind to */
    if ( getaddrinfo(NULL, multicastPort, &hints, &localAddr) != 0 ) {
	fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
	goto error;
    }
  

    /* Create socket for receiving datagrams */
    if ( (sock = socket(localAddr->ai_family, localAddr->ai_socktype, 0)) < 0 ) {
	perror("socket() failed");
	goto error;
    }
    
    
   
    /*
     * Enable SO_REUSEADDR to allow multiple instances of this
     * application to receive copies of the multicast datagrams.
     */
    if (setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof(int)) == -1) {
	perror("setsockopt");
	goto error;
    }
  
    /* Bind the local address to the multicast port */
    if ( bind(sock, localAddr->ai_addr, localAddr->ai_addrlen) != 0 ) {
	perror("bind() failed");
	goto error;
    }

    /* get/set socket receive buffer */
    int optval=0;
    socklen_t optval_len = sizeof(optval);
    int dfltrcvbuf;
    if(getsockopt(sock, SOL_SOCKET, SO_RCVBUF,(char*)&optval, &optval_len) !=0) {
	perror("getsockopt");
	goto error;
    }
    dfltrcvbuf = optval;
    optval = multicastRecvBufSize;
    if(setsockopt(sock,SOL_SOCKET,SO_RCVBUF,(char*)&optval,sizeof(optval)) != 0) {
	perror("setsockopt");
	goto error;
    }
    if(getsockopt(sock, SOL_SOCKET, SO_RCVBUF,(char*)&optval, &optval_len) != 0) {
	perror("getsockopt");
	goto error;
    }
    printf("tried to set socket receive buffer from %d to %d, got %d\n",
	   dfltrcvbuf, multicastRecvBufSize, optval);

  
    
    
    /* Loop through all multicast-capable interfaces */
    if (getifaddrs(&ifaddr) != 0) {
	perror("getifaddrs() failed");
	goto error;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
	unsigned int if_index;
	if (ifa->ifa_addr == NULL) continue;
	if (ifa->ifa_addr->sa_family != multicastAddr->ai_family) continue;
	if ((ifa->ifa_flags & IFF_MULTICAST) == 0) continue;

	if_index = if_nametoindex(ifa->ifa_name);
	if (if_index == 0) {
	    perror("if_nametoindex() failed");
	    goto error;
	}
	if (if_index_used[if_index]) continue;
	if_index_used[if_index] = 1;
	
	printf("Receiving on interface %s\n", ifa->ifa_name);

	/* Join the multicast group. We do this seperately depending on whether we
	 * are using IPv4 or IPv6. 
	 */
	if ( multicastAddr->ai_family  == PF_INET &&  
	     multicastAddr->ai_addrlen == sizeof(struct sockaddr_in) ) /* IPv4 */
	    {
		struct ip_mreq multicastRequest;  /* Multicast address join structure */

		/* Specify the multicast group */
		memcpy(&multicastRequest.imr_multiaddr,
		       &((struct sockaddr_in*)(multicastAddr->ai_addr))->sin_addr,
		       sizeof(multicastRequest.imr_multiaddr));

		/* Accept multicast on this interface */
		memcpy(&multicastRequest.imr_interface.s_addr,
		       &((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr,
		       sizeof(multicastRequest.imr_interface.s_addr));

		/* Join the multicast address */
		if ( setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0 ) {
		    perror("setsockopt() failed");
		    goto error;
		}
	    }
	else if ( multicastAddr->ai_family  == PF_INET6 &&
		  multicastAddr->ai_addrlen == sizeof(struct sockaddr_in6) ) /* IPv6 */
	    {
		struct ipv6_mreq multicastRequest;  /* Multicast address join structure */

		/* Specify the multicast group */
		memcpy(&multicastRequest.ipv6mr_multiaddr,
		       &((struct sockaddr_in6*)(multicastAddr->ai_addr))->sin6_addr,
		       sizeof(multicastRequest.ipv6mr_multiaddr));

		/* Accept multicast on this interface */
		multicastRequest.ipv6mr_interface = if_index;

		/* Join the multicast address */
		if ( setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0 ) {
		    perror("setsockopt() failed");
		    goto error;
		}
	    }
	else {
	    perror("Neither IPv4 or IPv6"); 
	    goto error;
	}
    }

    
    if(localAddr)
	freeaddrinfo(localAddr);
    if(multicastAddr)
	freeaddrinfo(multicastAddr);
    if (ifaddr)
	freeifaddrs(ifaddr);
    
    return sock;

 error:
    if(localAddr)
	freeaddrinfo(localAddr);
    if(multicastAddr)
	freeaddrinfo(multicastAddr);
    if (ifaddr)
	freeifaddrs(ifaddr);

    return -1;
}
