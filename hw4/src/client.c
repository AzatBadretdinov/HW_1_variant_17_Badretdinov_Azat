#include "net.h"
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifndef ROLE
#error ROLE is required
#endif
enum { STUDENT=1, TEACHER=2, WATCH=3 };
static int output(const char *line) {
    return write_all(STDOUT_FILENO,line,strlen(line)) || write_all(STDOUT_FILENO,"\n",1);
}
int main(int argc,char **argv) {
    const char *host=NULL;
    int port=0,id=0;
    for(int i=1;i<argc;++i) {
        const char *key=argv[i];
        if(!strcmp(key,"--help")){
            printf("%s --host IPv4 --port 1..65535%s\n",argv[0],ROLE==WATCH?"":" --id 1..32");
            return 0;
        }
        if(++i==argc)goto invalid;
        const char *val=argv[i];
        if(!strcmp(key,"--host")&&*val)host=val;
        else if(!strcmp(key,"--port")&&parse_number(val,1,65535,&port)){}
        else if(ROLE!=WATCH&&!strcmp(key,"--id")&&parse_number(val,1,LIMIT,&id)){}
        else goto invalid;
    }
    if(!host||!port||(ROLE!=WATCH&&!id))goto invalid;
    if(signals_init()){perror("signals");return 2;}
    int fd=tcp_connect(host,port);
    if(fd<0){perror("connect");return 2;}
    Connection c;
    connection_init(&c,fd);
    const char *role=ROLE==STUDENT?"STUDENT":ROLE==TEACHER?"TEACHER":"WATCH";
    net_queue(&c,"HELLO %s %d\n",role,id);
    int result=0,preparing=0,ticket=0,job_student=0,job_grade=0,task_active=0;
    unsigned job=0;
    int64_t deadline=0;
    while(!interrupted) {
        if(task_active&&clock_ms()>=deadline) {
            if(preparing)net_queue(&c,"READY %d\n",ticket);
            else net_queue(&c,"GRADE %d %d %u\n",job_student,job_grade,job);
            task_active=0;
        }
        net_flush(&c);
        if(c.bad){result=2;break;}
        struct pollfd p={.fd=fd,.events=POLLIN};
        if(c.tx_len>c.tx_start)p.events|=POLLOUT;
        int rc=poll(&p,1,10);
        if(rc<0){if(interrupted)break;result=2;break;}
        if(p.revents&POLLOUT)net_flush(&c);
        if(p.revents&POLLIN) {
            if(net_read(&c)<0){result=2;break;}
            char line[LINE_SIZE]; int got;
            while((got=net_line(&c,line))!=0) {
                if(got<0){result=2;goto done;}
                if(output(line)){result=2;goto done;}
                if(!strncmp(line,"BYE ",4)||!strcmp(line,"DONE"))goto done;
                if(!strncmp(line,"ERROR ",6)){result=2;goto done;}
                if(ROLE==WATCH)continue;
                int grade; int64_t delay; char extra;
                if(ROLE==STUDENT&&sscanf(line,"PREPARE %d %" SCNd64 " %c",&ticket,&delay,&extra)==2&&delay>0) {
                    preparing=task_active=1;deadline=clock_ms()+delay;
                } else if(ROLE==TEACHER&&sscanf(line,"CHECK %d %d %d %" SCNd64 " %u %c",
                          &job_student,&ticket,&job_grade,&delay,&job,&extra)==5&&delay>0) {
                    preparing=0;task_active=1;deadline=clock_ms()+delay;
                } else if(ROLE==STUDENT&&sscanf(line,"RESULT %d %c",&grade,&extra)==1&&grade>=2&&grade<=5) {
                    net_queue(&c,"ACK %d\n",grade);
                } else if(ROLE==STUDENT&&!strcmp(line,"REJECT"))net_queue(&c,"ACK 0\n");
                else if(strcmp(line,"WAIT")&&strcmp(line,"WELCOME")){result=2;goto done;}
            }
        }
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL)){result=2;break;}
    }
done:
    close(fd);
    return interrupted?128+interrupted:result;
invalid:
    fprintf(stderr,"Некорректные параметры. Используйте --help.\n");return 2;
}
