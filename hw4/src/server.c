#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
enum { UNKNOWN, STUDENT, TEACHER, WATCH };
enum State { NEW, PREPARING, READY, QUEUED, CHECKING, GRADED, DONE, REJECTED };
static const char *states[] = {"new","preparing","ready","queued","checking","graded","done","rejected"};
typedef struct {
    int ticket, preparation, planned_grade, grade, teacher, peer, confirmed, waited;
    enum State state;
    uint32_t job;
} Student;
typedef struct { int peer, student; } Teacher;
typedef struct { Connection c; int role, id; int64_t connected; } Peer;
typedef struct {
    int students, teachers, tickets, capacity, prep_min, prep_max, check, unit, max_time, seed;
    int sequential, ticket_grade;
    const char *host, *log;
    int port;
} Config;
typedef struct {
    Config config;
    Peer peers[PEERS];
    Student students[LIMIT];
    Teacher teachers[LIMIT];
    int fifo[LIMIT], head, len, admitted, completed, settled, peak, waited, grade_sum;
    int listener, log, io_error, monitors;
    uint64_t sequence;
    uint32_t jobs;
    int64_t start;
} Server;
static void event(Server *s, const char *format, ...) {
    char payload[384], row[LINE_SIZE];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(payload, sizeof payload, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof payload) { s->io_error = 1; return; }
    int len = snprintf(row, sizeof row, "EVENT %" PRIu64 " [ms=%" PRId64 "] %s\n",
                       ++s->sequence, clock_ms() - s->start, payload);
    if (len < 0 || (size_t)len >= sizeof row) { s->io_error = 1; return; }
    int a = write_all(STDOUT_FILENO, row, (size_t)len);
    int b = write_all(s->log, row, (size_t)len);
    if (a || b) s->io_error = 1;
    for (int i = 0; i < PEERS; ++i)
        if (s->peers[i].c.fd >= 0 && s->peers[i].role == WATCH)
            net_queue(&s->peers[i].c, "%s", row);
}
static uint32_t random_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}
static void inputs(Server *s) {
    Config *c = &s->config;
    int tickets[10000];
    uint32_t rng = (uint32_t)c->seed;
    s->admitted = c->students < c->tickets ? c->students : c->tickets;
    for (int i = 0; i < c->tickets; ++i) tickets[i] = i + 1;
    if (!c->sequential)
        for (int i = c->tickets - 1; i > 0; --i) {
            int j = (int)(random_next(&rng) % (uint32_t)(i + 1));
            int temp = tickets[i]; tickets[i] = tickets[j]; tickets[j] = temp;
        }
    for (int i = 0; i < c->students; ++i) {
        Student *student = &s->students[i];
        student->peer = -1; student->teacher = -1;
        if (i >= s->admitted) { student->state = REJECTED; continue; }
        student->ticket = tickets[i];
        student->preparation = c->prep_min + (int)(random_next(&rng) % (uint32_t)(c->prep_max-c->prep_min+1));
        uint32_t grade_seed = random_next(&rng);
        student->planned_grade = c->ticket_grade ? 2 + (student->ticket-1)%4 : 2 + (int)(grade_seed%4);
    }
    for (int i = 0; i < c->teachers; ++i)
        s->teachers[i] = (Teacher){.peer=-1, .student=-1};
    for (int i = 0; i < PEERS; ++i) connection_init(&s->peers[i].c, -1);
}
static int64_t duration(Server *s, int units) { return (int64_t)units * s->config.unit; }
static void snapshot(Server *s, Peer *p) {
    net_queue(&p->c, "SNAPSHOT %" PRIu64 " students=%d teachers=%d completed=%d queue=%d/%d\n",
              s->sequence, s->config.students, s->config.teachers, s->completed, s->len, s->config.capacity);
    for (int i = 0; i < s->config.students; ++i) {
        Student *st = &s->students[i];
        net_queue(&p->c, "STATE student=%d ticket=%d state=%s teacher=%d grade=%d online=%d\n",
                  i+1, st->ticket, states[st->state], st->teacher+1, st->grade, st->peer>=0);
    }
    for (int i = 0; i < s->config.teachers; ++i)
        net_queue(&p->c, "STATE teacher=%d student=%d online=%d\n", i+1,
                  s->teachers[i].student+1, s->teachers[i].peer>=0);
    net_queue(&p->c, "END_SNAPSHOT %" PRIu64 "\n", s->sequence);
}
static void resume_student(Server *s, Peer *p) {
    Student *st = &s->students[p->id];
    switch (st->state) {
    case NEW:
        st->state = PREPARING;
        event(s, "БИЛЕТ student=%d ticket=%d preparation=%d", p->id+1, st->ticket, st->preparation);
        // fall through
    case PREPARING:
        net_queue(&p->c, "PREPARE %d %" PRId64 "\n", st->ticket, duration(s,st->preparation)); break;
    case GRADED:
        net_queue(&p->c, "RESULT %d\n", st->grade); break;
    case DONE:
        net_queue(&p->c, "DONE\n"); p->c.closing = 1; break;
    case REJECTED:
        if (st->confirmed) { net_queue(&p->c,"DONE\n"); p->c.closing=1; }
        else net_queue(&p->c,"REJECT\n");
        break;
    default: net_queue(&p->c, "WAIT\n"); break;
    }
}
static void disconnect_peer(Server *s, int i) {
    Peer *p = &s->peers[i];
    if (p->c.fd < 0) return;
    int role = p->role, id = p->id;
    close(p->c.fd);
    connection_init(&p->c,-1);
    p->role = UNKNOWN;
    if (role == STUDENT) s->students[id].peer = -1;
    if (role == TEACHER) {
        Teacher *t = &s->teachers[id];
        t->peer = -1;
        if (t->student >= 0) {
            Student *st = &s->students[t->student];
            st->state = READY; st->teacher = -1;
            event(s, "ВОЗВРАТ student=%d: преподаватель отключился", t->student+1);
            t->student = -1;
        }
    }
    if (role == WATCH) --s->monitors;
    if (role) event(s, "ОТКЛЮЧЕНИЕ role=%s id=%d",
                    role==STUDENT?"student":role==TEACHER?"teacher":"watch", id+1);
}
static void protocol_error(Peer *p, const char *reason) {
    net_queue(&p->c, "ERROR %s\n", reason);
    p->c.closing=1;
}
static int hello(Server *s, int index, const char *line) {
    Peer *p=&s->peers[index];
    char role[16], extra;
    unsigned id;
    if (sscanf(line,"HELLO %15s %u %c",role,&id,&extra)!=2) return -1;
    if (!strcmp(role,"WATCH") && id==0) {
        if (s->monitors>=LIMIT) return -1;
        p->role=WATCH; p->id=0; ++s->monitors;
        snapshot(s,p);
    } else if (!strcmp(role,"STUDENT") && id>=1 && id<=(unsigned)s->config.students) {
        if (s->students[id-1].peer>=0) return -1;
        p->role=STUDENT; p->id=(int)id-1; s->students[id-1].peer=index;
        resume_student(s,p);
    } else if (!strcmp(role,"TEACHER") && id>=1 && id<=(unsigned)s->config.teachers) {
        if (s->teachers[id-1].peer>=0) return -1;
        p->role=TEACHER; p->id=(int)id-1; s->teachers[id-1].peer=index;
        net_queue(&p->c,"WELCOME\n");
    } else return -1;
    event(s,"ПОДКЛЮЧЕНИЕ role=%s id=%u",role,id);
    return 0;
}
static int command(Server *s, int index, const char *line) {
    Peer *p=&s->peers[index];
    if (!p->role) return hello(s,index,line);
    if (!strcmp(line,"QUIT")) { p->c.closing=1; return 0; }
    if (p->role==WATCH) return -1;
    int value; char extra;
    if (p->role==STUDENT) {
        Student *st=&s->students[p->id];
        if (sscanf(line,"READY %d %c",&value,&extra)==1 && value==st->ticket && st->state==PREPARING) {
            st->state=READY;
            event(s,"ГОТОВ student=%d",p->id+1); return 0;
        }
        if (sscanf(line,"ACK %d %c",&value,&extra)==1) {
            if (st->state==GRADED && value==st->grade) {
                st->state=DONE; ++s->settled; ++s->completed; s->grade_sum+=value;
                event(s,"ОЦЕНКА student=%d grade=%d",p->id+1,value);
            } else if (st->state==REJECTED && !st->confirmed && value==0) {
                st->confirmed=1; ++s->settled;
                event(s,"ОТКАЗ student=%d: нет уникального билета",p->id+1);
            } else return -1;
            net_queue(&p->c,"DONE\n"); p->c.closing=1; return 0;
        }
    } else {
        int student,grade;
        unsigned job;
        Teacher *teacher=&s->teachers[p->id];
        if (sscanf(line,"GRADE %d %d %u %c",&student,&grade,&job,&extra)==3 &&
            student>=1 && student<=s->config.students && teacher->student==student-1) {
            Student *st=&s->students[student-1];
            if (st->state!=CHECKING || st->job!=job || st->planned_grade!=grade) return -1;
            st->state=GRADED; st->grade=grade; teacher->student=-1;
            event(s,"ПРОВЕРЕНО teacher=%d student=%d grade=%d job=%u",p->id+1,student,grade,job);
            if (st->peer>=0) net_queue(&s->peers[st->peer].c,"RESULT %d\n",grade);
            return 0;
        }
    }
    return -1;
}
static void dispatch(Server *s) {
    for (;;) {
        for (int t=0;t<s->config.teachers && s->len;++t) {
            Teacher *teacher=&s->teachers[t];
            if (teacher->peer<0 || teacher->student>=0 || s->peers[teacher->peer].c.bad ||
                s->peers[teacher->peer].c.closing) continue;
            int id=s->fifo[s->head]; s->head=(s->head+1)%LIMIT; --s->len;
            Student *st=&s->students[id];
            st->state=CHECKING; st->teacher=t; st->job=++s->jobs; teacher->student=id;
            net_queue(&s->peers[teacher->peer].c,"CHECK %d %d %d %" PRId64 " %u\n",
                      id+1,st->ticket,st->planned_grade,duration(s,s->config.check),st->job);
            event(s,"ПРОВЕРКА teacher=%d student=%d job=%u",t+1,id+1,st->job);
        }
        int found=0;
        for (int id=0;id<s->config.students;++id) {
            Student *st=&s->students[id];
            if (st->state!=READY) continue;
            if (s->len==s->config.capacity) {
                if (!st->waited) { st->waited=1; ++s->waited; event(s,"ОЖИДАНИЕ student=%d: очередь заполнена",id+1); }
                continue;
            }
            s->fifo[(s->head+s->len)%LIMIT]=id; ++s->len;
            st->state=QUEUED; found=1;
            if(s->len>s->peak)s->peak=s->len;
            event(s,"ОЧЕРЕДЬ student=%d size=%d capacity=%d",id+1,s->len,s->config.capacity);
        }
        if(!found)break;
    }
}
static void accept_connections(Server *s) {
    for (int count=0;count<PEERS;++count) {
        int fd=accept(s->listener,NULL,NULL);
        if(fd<0) { if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)s->io_error=1; return; }
        if(nonblocking(fd)) { close(fd); continue; }
        int index=-1;
        for(int i=0;i<PEERS;++i)if(s->peers[i].c.fd<0){index=i;break;}
        if(index<0){close(fd);continue;}
        Peer *p=&s->peers[index];
        connection_init(&p->c,fd); p->role=UNKNOWN; p->id=0; p->connected=clock_ms();
    }
}
static const char *run(Server *s) {
    for (;;) {
        if(interrupted)return "interrupted";
        if(s->io_error)return "io_error";
        if(s->settled==s->config.students)return "complete";
        if(s->config.max_time && clock_ms()-s->start>=duration(s,s->config.max_time))return "timeout";
        for(int i=0;i<PEERS;++i) {
            Peer *p=&s->peers[i];
            if(p->c.fd<0)continue;
            if(p->c.bad || (p->c.closing && p->c.tx_start==p->c.tx_len) ||
               (!p->role && clock_ms()-p->connected>2000))disconnect_peer(s,i);
        }
        struct pollfd fds[PEERS+1];
        fds[0]=(struct pollfd){.fd=s->listener,.events=POLLIN};
        for(int i=0;i<PEERS;++i) {
            Connection *c=&s->peers[i].c;
            fds[i+1]=(struct pollfd){.fd=c->fd,.events=POLLIN};
            if(c->tx_len>c->tx_start)fds[i+1].events|=POLLOUT;
        }
        int rc=poll(fds,PEERS+1,10);
        if(rc<0){if(errno==EINTR)continue;return "poll_error";}
        if(fds[0].revents&POLLIN)accept_connections(s);
        for(int i=0;i<PEERS;++i) {
            Peer *p=&s->peers[i];
            if(p->c.fd<0)continue;
            short re=fds[i+1].revents;
            if(re&POLLOUT)net_flush(&p->c);
            if((re&POLLIN) && !p->c.closing) {
                if(net_read(&p->c)<0)p->c.bad=1;
                else {
                    char line[LINE_SIZE]; int got;
                    int budget=32;
                    while(!p->c.closing && budget-- && (got=net_line(&p->c,line))!=0) {
                        if(got<0 || command(s,i,line)<0){protocol_error(p,"invalid_message_or_duplicate_id");break;}
                    }
                }
            }
            if(re&(POLLERR|POLLHUP|POLLNVAL))p->c.bad=1;
            if(p->c.bad)disconnect_peer(s,i);
        }
        dispatch(s);
    }
}
static void shutdown_server(Server *s,const char *reason) {
    close(s->listener);
    for(int i=0;i<s->config.students;++i) {
        Student *st=&s->students[i];
        event(s,"СТУДЕНТ id=%d ticket=%d state=%s teacher=%d grade=%d",
              i+1,st->ticket,states[st->state],st->teacher+1,st->grade);
    }
    event(s,"ИТОГ reason=%s admitted=%d completed=%d unfinished=%d rejected=%d peak_queue=%d waited=%d average=%.2f",
          reason,s->admitted,s->completed,s->admitted-s->completed,s->config.students-s->admitted,
          s->peak,s->waited,s->completed?(double)s->grade_sum/s->completed:0.0);
    for(int i=0;i<PEERS;++i)
        if(s->peers[i].c.fd>=0)net_queue(&s->peers[i].c,"BYE %s\n",reason);
    // Bounded flush: a slow/disconnected observer cannot delay termination indefinitely.
    int64_t until=clock_ms()+300;
    while(clock_ms()<until) {
        int pending=0;
        for(int i=0;i<PEERS;++i) {
            Connection *c=&s->peers[i].c;
            if(c->fd<0)continue;
            net_flush(c);
            if(c->bad || c->tx_start==c->tx_len){close(c->fd);c->fd=-1;}
            else pending=1;
        }
        if(!pending)break;
        poll(NULL,0,2);
    }
    for(int i=0;i<PEERS;++i)if(s->peers[i].c.fd>=0)close(s->peers[i].c.fd);
}
static void help(void) {
    puts("exam-server --host IPv4 --port 0..65535 [options]\n"
         "--students 0..32 (6) --teachers 1..32 (2) --tickets 0..10000 (6)\n"
         "--queue 1..32 (3) --prep-min 1..100000 (2) --prep-max 1..100000 (5)\n"
         "--check-time 1..100000 (2) --delay-ms 0..60000 (10; 0=1ms)\n"
         "--max-time 0..100000 (10000; 0=unlimited) --seed 1..2147483647 (17)\n"
         "--ticket-mode random|sequential --grade-mode random|ticket\n"
         "--log PATH (exam.log). Port 0 asks the OS to select a free port.");
}
int main(int argc,char **argv) {
    Config c={.students=6,.teachers=2,.tickets=6,.capacity=3,.prep_min=2,.prep_max=5,
              .check=2,.unit=10,.max_time=10000,.seed=17,.log="exam.log",.port=-1};
    struct Option{const char *key;int *dst,min,max;} opt[]={
        {"--port",&c.port,0,65535},{"--students",&c.students,0,LIMIT},{"--teachers",&c.teachers,1,LIMIT},
        {"--tickets",&c.tickets,0,10000},{"--queue",&c.capacity,1,LIMIT},
        {"--prep-min",&c.prep_min,1,100000},{"--prep-max",&c.prep_max,1,100000},
        {"--check-time",&c.check,1,100000},{"--delay-ms",&c.unit,0,60000},
        {"--max-time",&c.max_time,0,100000},{"--seed",&c.seed,1,2147483647}};
    for(int i=1;i<argc;++i) {
        const char *key=argv[i];
        if(!strcmp(key,"--help")){help();return 0;}
        if(++i==argc)goto invalid;
        const char *val=argv[i];
        if(!strcmp(key,"--host") && *val){c.host=val;continue;}
        if(!strcmp(key,"--log") && *val){c.log=val;continue;}
        if(!strcmp(key,"--ticket-mode") && (!strcmp(val,"random")||!strcmp(val,"sequential"))){c.sequential=!strcmp(val,"sequential");continue;}
        if(!strcmp(key,"--grade-mode") && (!strcmp(val,"random")||!strcmp(val,"ticket"))){c.ticket_grade=!strcmp(val,"ticket");continue;}
        int ok=0;
        for(size_t j=0;j<sizeof opt/sizeof opt[0];++j)
            if(!strcmp(key,opt[j].key))ok=parse_number(val,opt[j].min,opt[j].max,opt[j].dst);
        if(!ok)goto invalid;
    }
    if(!c.host || c.port<0 || c.prep_min>c.prep_max)goto invalid;
    if(!c.unit)c.unit=1;
    if(signals_init()){perror("signals");return 2;}
    Server *s=calloc(1,sizeof *s);
    if(!s){perror("calloc");return 2;}
    s->config=c; inputs(s); s->start=clock_ms();
    s->listener=tcp_listen(c.host,c.port,&s->config.port);
    if(s->listener<0){perror("listen");free(s);return 2;}
    s->log=open(c.log,O_WRONLY|O_CREAT|O_TRUNC|O_NONBLOCK,0644);
    if(s->log<0){perror("log");close(s->listener);free(s);return 2;}
    event(s,"LISTEN host=%s port=%d students=%d teachers=%d tickets=%d queue=%d seed=%d unit_ms=%d",
          c.host,s->config.port,c.students,c.teachers,c.tickets,c.capacity,c.seed,c.unit);
    const char *reason=run(s);
    shutdown_server(s,reason);
    int result=(strcmp(reason,"complete")&&strcmp(reason,"timeout"))?2:0;
    if(interrupted)result=128+interrupted;
    if(close(s->log)||s->io_error)result=2;
    free(s); return result;
invalid:
    fprintf(stderr,"Некорректные параметры. Используйте --help.\n");return 2;
}
