#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/uio.h>
#include <ext_socket.h>
#ifdef __APPLE__
#include "inet_ntop.c"
#endif
#include <string.h>

#define BUFLEN        100
#define MAX_OUTGOING  128
#define MAX_INCOMING  128


static int checkIPv6()
{
   int sd = ext_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
   if(sd >= 0) {
      ext_close(sd);
      return(1);
   }
   return(0);
}


static void handle_event(void *buf)
{
   struct sctp_assoc_change *sac;
   struct sctp_send_failed  *ssf;
   struct sctp_paddr_change *spc;
   struct sctp_remote_error *sre;
   union sctp_notification  *snp;
   char addrbuf[INET6_ADDRSTRLEN];
   const char *ap;
   struct sockaddr_in  *sin;
   struct sockaddr_in6 *sin6;

   snp = buf;

   switch(snp->sn_header.sn_type) {
      case SCTP_ASSOC_CHANGE:
         sac = &snp->sn_assoc_change;
         printf("^^^ assoc_change: state=%hu, error=%hu, instr=%hu, outstr=%hu\n",
                 sac->sac_state, sac->sac_error, sac->sac_inbound_streams, sac->sac_outbound_streams);
       break;
      case SCTP_SEND_FAILED:
         ssf = &snp->sn_send_failed;
         printf("^^^ sendfailed: len=%hu err=%d\n", ssf->ssf_length, ssf->ssf_error);
       break;
      case SCTP_PEER_ADDR_CHANGE:
         spc = &snp->sn_paddr_change;
         if(((struct sockaddr_in *)&spc->spc_aaddr)->sin_family == AF_INET) {
           sin = (struct sockaddr_in *)&spc->spc_aaddr;
           ap = inet_ntop(AF_INET, &sin->sin_addr, addrbuf, INET6_ADDRSTRLEN);
         } else {
           sin6 = (struct sockaddr_in6 *)&spc->spc_aaddr;
           ap = inet_ntop(AF_INET6, &sin6->sin6_addr, addrbuf, INET6_ADDRSTRLEN);
         }
         printf("^^^ intf_change: %s state=%d, error=%d\n", ap, spc->spc_state, spc->spc_error);
       break;
      case SCTP_REMOTE_ERROR:
         sre = &snp->sn_remote_error;
         printf("^^^ remote_error: err=%hu\n", ntohs(sre->sre_error));
       break;
      case SCTP_SHUTDOWN_EVENT:
         printf("^^^ shutdown event\n");
       break;
      default:
         printf("unknown type: %hu\n", snp->sn_header.sn_type);
       break;
   }
}


static void echo(int fd, int socketModeUDP)
{
   ssize_t                  nr;
   struct  sctp_sndrcvinfo  sri;
   char                     buf[BUFLEN];
   size_t                   buflen;
   ssize_t                  received;
   int                      flags;
   unsigned int             incoming = 1;
   unsigned int             outgoing = 1;
   union sctp_notification *snp;

   /* Wait for something to echo */
   buflen = sizeof(buf);
   flags  = 0;
   received = sctp_recvmsg(fd, buf, &buflen, NULL, 0, &sri, &flags);
   while(received > 0) {
      /* Intercept notifications here */
      if(flags & MSG_NOTIFICATION) {
         snp = (union sctp_notification *)buf;
         if(snp->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
            incoming = snp->sn_assoc_change.sac_inbound_streams;
            outgoing = snp->sn_assoc_change.sac_outbound_streams;
         }
         handle_event(buf);
      }
      else {
         printf("got %u bytes on stream %hu:\n", received, sri.sinfo_stream);
         fflush(stdout);
         write(0, buf, received);

         /* Echo it back */
         if(sctp_sendmsg(fd, buf, received, NULL, 0,
                         0x29091976, 0,
                         sri.sinfo_stream % outgoing,
                         0, 0) < 0) {
            perror("sendmsg");
            exit(1);
         }
      }

      buflen = sizeof(buf);
      flags  = 0;
      received = sctp_recvmsg(fd, buf, &buflen, NULL, 0, &sri, &flags);
   }

   if(nr < 0) {
      perror("recvmsg");
   }
   if(socketModeUDP == 0) {
      ext_close(fd);
   }
}


int main()
{
   int                         lfd, cfd;
   struct sctp_event_subscribe events;
   struct sctp_initmsg         init;
   struct sockaddr_in          sin[1];

   if((lfd = ext_socket(checkIPv6() ? AF_INET6 : AF_INET,
                        SOCK_STREAM, IPPROTO_SCTP)) == -1) {
      perror("socket");
      exit(1);
   }

   init.sinit_num_ostreams   = MAX_OUTGOING;
   init.sinit_max_instreams  = MAX_INCOMING;
   init.sinit_max_attempts   = 3;
   init.sinit_max_init_timeo = 30;
   if(ext_setsockopt(lfd, IPPROTO_SCTP, SCTP_INITMSG, (void*)&init, sizeof(init)) < 0) {
      perror("setsockopt");
      exit(1);
   }

   sin->sin_family      = AF_INET;
   sin->sin_port        = htons(7);
   sin->sin_addr.s_addr = INADDR_ANY;
   if(ext_bind(lfd, (struct sockaddr *)sin, sizeof (*sin)) == -1) {
      perror("bind");
      exit(1);
   }

   /* Enable ancillary data and notifications */
   memset((char*)&events,1,sizeof(events));
   if (ext_setsockopt(lfd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
     perror("setsockopt SCTP_EVENTS");
     exit(1);
   }

   if(ext_listen(lfd, 1) == -1) {
      perror("listen");
      exit(1);
   }

   /* Wait for new associations */
   for (;;) {
     if((cfd = ext_accept(lfd, NULL, 0)) == -1) {
        perror("accept");
        exit(1);
     }

     /* Enable ancillary data and notifications */
     memset((char*)&events,1,sizeof(events));
     if(ext_setsockopt(cfd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
        perror("setsockopt SCTP_EVENTS");
        exit(1);
     }

     /* Echo back any and all data */
     echo(cfd,0);
   }
}
