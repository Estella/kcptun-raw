#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <linux/netfilter_ipv4.h>
#include <ev.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>

#include "ikcp.h"
#include "trans_packet.h"

#include "common.h"


unsigned int getclock() {
  struct timeval te; 
  gettimeofday(&te, NULL); // get current time
  long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // caculate milliseconds
  return milliseconds;
}

int setnonblocking(int fd) {
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
    flags = 0;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int packet_output(const char* buf, int len, ikcpcb *kcp, void *user) {
  struct connection_info* connection = (struct connection_info*)user;
  char* send_buf = malloc(len + 8);

  memcpy(send_buf, CONNECTION_PUSH, 8);

  memcpy(send_buf + 8, buf, len);


  int ret = send_packet(&packetinfo, send_buf, len + 8, connection->conv);

  free(send_buf);

  return ret;
}

void read_cb(struct ev_loop *loop, struct ev_io *w_, int revents) {

  struct connection_info *connection = ((struct io_wrap*)w_)->connection;
  struct ev_io *watcher = &(((struct io_wrap*)w_)->io);

  char buffer[BUFFER_SIZE];

  if(EV_ERROR & revents) {
    return;
  }

  if (iqueue_get_len(&((connection->kcp)->snd_queue)) > 20) {
    return;
  }

  int recv_len = recv(watcher->fd, buffer, BUFFER_SIZE, 0);

  if((recv_len == -1 && errno != EAGAIN && errno != EWOULDBLOCK) || recv_len == 0) {
    printf("conv %d recv ends.\n", connection->conv);
    close_connection(connection);
    notify_remote_close(connection);
    return;
  }

  if (recv_len == -1) {
    return;
  }

  // printf("Received %d bytes from local. conv=%d\n", recv_len, connection->conv);

  if (ikcp_send(connection->kcp, buffer, recv_len) < 0) {
    printf("kcp send error.\n");
    close_connection(connection);
    notify_remote_close(connection);
    return;
  }

  kcp_update_interval();

}

void write_cb(struct ev_loop *loop, struct ev_io *w_, int revents) {

  struct connection_info *connection = ((struct io_wrap*)w_)->connection;
  struct ev_io *watcher = &(((struct io_wrap*)w_)->io);

  if(EV_ERROR & revents) {
    return;
  }

  if (connection->pending_send_buf_len == 0 || connection->pending_send_buf == NULL) {
    return;
  }

  int sent_byte = send(watcher->fd, connection->pending_send_buf, connection->pending_send_buf_len, 0);

  if (sent_byte == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    printf("send error.\n");
    close_connection(connection);
    notify_remote_close(connection);
    return;
  }

  if (sent_byte == -1) {
    return;
  }

  if (sent_byte < connection->pending_send_buf_len) {
    // printf("[write_cb()]localfd: Pending send.\n");
    memmove(connection->pending_send_buf, connection->pending_send_buf + sent_byte, connection->pending_send_buf_len - sent_byte);
  } else if (sent_byte == connection->pending_send_buf_len) {
    free(connection->pending_send_buf);
    connection->pending_send_buf = NULL;
    ev_io_stop(loop, watcher);
  }

  connection->pending_send_buf_len -= sent_byte;

}

void kcp_update_timer_cb(struct ev_loop *loop, struct ev_timer* timer, int revents) {
  kcp_update_interval();
  ev_timer_again(loop, timer);
}


void kcp_update_interval() {
  char recv_buffer[BUFFER_SIZE];

  for (int i=0; i<MAX_CONNECTIONS; i++) {
    if (connection_queue[i].in_use == 1 && connection_queue[i].kcp != NULL) {
      ikcp_update(connection_queue[i].kcp, getclock());

      if (connection_queue[i].pending_send_buf_len > BUFFER_SIZE * 20) {
        continue;
      }

      int recv_len = ikcp_recv(connection_queue[i].kcp, recv_buffer, BUFFER_SIZE);
      if (recv_len > 0) {

        // printf("Received %d bytes from kcp. conv=%d\n", recv_len,connection_queue[i].conv);

        int sent_byte = 0;

        if (connection_queue[i].pending_send_buf_len == 0) {
          sent_byte = send(connection_queue[i].local_fd, recv_buffer, recv_len, 0);
        }

        if (sent_byte == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          close_connection(&(connection_queue[i]));
          notify_remote_close(&(connection_queue[i]));
          continue;
        }

        if (sent_byte == -1) {
          sent_byte = 0;
        }

        if (sent_byte < recv_len) {
          // printf("[kcp_update_interval()]localfd: Pending send.\n");
          
          if (connection_queue[i].pending_send_buf_len == 0) {
            connection_queue[i].pending_send_buf = malloc(recv_len - sent_byte);
            memcpy(connection_queue[i].pending_send_buf, recv_buffer + sent_byte, recv_len - sent_byte);
            connection_queue[i].pending_send_buf_len = recv_len - sent_byte;
          } else {
            connection_queue[i].pending_send_buf = realloc(connection_queue[i].pending_send_buf, connection_queue[i].pending_send_buf_len + recv_len - sent_byte);
            memcpy(connection_queue[i].pending_send_buf + connection_queue[i].pending_send_buf_len, recv_buffer + sent_byte, recv_len - sent_byte);
            connection_queue[i].pending_send_buf_len += recv_len - sent_byte;
          }
          ev_io_start(loop, &(connection_queue[i].write_io.io));
        }

        if (sent_byte == recv_len) {
          // printf("localfd: Sent immediately.\n");
        }

      }
    }
  }
    
}

void notify_remote_close(struct connection_info* connection) {
  printf("Notifying remote to close.\n");
  send_packet(&packetinfo, CONNECTION_CLOSE, 8, connection->conv);
}

void close_connection(struct connection_info* connection) {

  if (connection->in_use == 0) {
    return;
  }

  printf("Closing connection.conv=%d.\n", connection->conv);

  ev_io_stop(loop, &((connection->read_io).io));
  ev_io_stop(loop, &((connection->write_io).io));

  close(connection->local_fd);

  ikcp_release(connection->kcp);
  connection->kcp = NULL;

  if (connection->pending_send_buf != NULL) {
    free(connection->pending_send_buf);
  }

  connection->pending_send_buf = NULL;

  connection->pending_send_buf_len = 0;

  connection->in_use = 0;
}

void packet_read_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
  if(EV_ERROR & revents) {
    return;
  }
  check_packet_recv(&packetinfo);
  kcp_update_interval();
}

int is_packet_command(char* packet_buffer, const char* command){
  for (int i=0; i<8; i++) {
    if (packet_buffer[i] != command[i]) {
      return 0;
    }
  }
  return 1;
}

void heart_beat_timer_cb(struct ev_loop *loop, struct ev_timer* timer, int revents) {
  send_packet(&packetinfo, HEART_BEAT, 8, 0);

  ev_timer_again(loop, timer);
}

int iqueue_get_len(struct IQUEUEHEAD* queue) {
  struct IQUEUEHEAD* head = queue;
  struct IQUEUEHEAD* node = queue->next;
  int ret = 0;
  while (node != head) {
    ret++;
    node = node->next;
  }
  return ret;
}

void init_kcp_mode(int argc, char* argv[]) {
  char* last_arg = argv[argc - 1];

  kcpconfig.nodelay = 1;
  kcpconfig.interval = 10;
  kcpconfig.resend = 2;
  kcpconfig.nc = 1;

  if (!strcmp(last_arg, "normal")) {
    printf("normal mode enabled.\n");
    kcpconfig.nodelay = 0;
    kcpconfig.interval = 30;
    kcpconfig.resend = 2;
    kcpconfig.nc = 1;
  } else if (!strcmp(last_arg, "fast")) {
    printf("fast mode enabled.\n");
    kcpconfig.nodelay = 0;
    kcpconfig.interval = 20;
    kcpconfig.resend = 2;
    kcpconfig.nc = 1;
  } else if (!strcmp(last_arg, "fast2")) {
    printf("fast2 mode enabled.\n");
    kcpconfig.nodelay = 1;
    kcpconfig.interval = 20;
    kcpconfig.resend = 2;
    kcpconfig.nc = 1;
  } else if (!strcmp(last_arg, "fast3")) {
    printf("fast3 mode enabled.\n");
    kcpconfig.nodelay = 1;
    kcpconfig.interval = 10;
    kcpconfig.resend = 2;
    kcpconfig.nc = 1;
  }

}