#include "net.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    assert(!signals_init());
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(!nonblocking(pair[0]) && !nonblocking(pair[1]));
    int small=1024;
    assert(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof small));
    static Connection sender,receiver;
    connection_init(&sender,pair[0]); connection_init(&receiver,pair[1]);
    char text[30001];
    memset(text,'A',sizeof text-1); text[sizeof text-1]=0;
    assert(!net_queue(&sender,"%s",text));
    assert(!net_flush(&sender));
    assert(sender.tx_start>0 && sender.tx_start<sender.tx_len);
    size_t total=0;
    char bytes[4096];
    for(int attempts=0;attempts<1000 && total<30000;++attempts) {
        ssize_t n;
        while((n=recv(pair[1],bytes,sizeof bytes,0))>0) {
            for(ssize_t i=0;i<n;++i)assert(bytes[i]=='A');
            total+=(size_t)n;
        }
        assert(errno==EAGAIN || errno==EWOULDBLOCK);
        assert(!net_flush(&sender));
    }
    assert(total==30000 && sender.tx_len==0 && sender.tx_start==0);
    assert(write_all(pair[0],"HEL",3)==0);
    assert(net_read(&receiver)==1);
    char line[LINE_SIZE];
    assert(net_line(&receiver,line)==0);
    assert(write_all(pair[0],"LO\nTWO\n",7)==0);
    assert(net_read(&receiver)==1);
    assert(net_line(&receiver,line)==1 && !strcmp(line,"HELLO"));
    assert(net_line(&receiver,line)==1 && !strcmp(line,"TWO"));
    assert(net_line(&receiver,line)==0);
    assert(net_queue(&sender,"%s%s",text,text)<0 && sender.bad);
    close(pair[0]); close(pair[1]);
    puts("PASS partial_send_exact_bytes");
    puts("PASS fragmented_and_joined_frames");
    puts("PASS bounded_output_buffer");
    return 0;
}
