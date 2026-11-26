#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
int main(){int s=socket(AF_INET6,SOCK_STREAM,0);struct sockaddr_in6 a={0},b={0};a.sin6_family=AF_INET6;a.sin6_scope_id=if_nametoindex("iap2ncm0");inet_pton(AF_INET6,"fe80::1",&a.sin6_addr);a.sin6_port=0;bind(s,(void*)&a,sizeof(a));b.sin6_family=AF_INET6;b.sin6_scope_id=a.sin6_scope_id;inet_pton(AF_INET6,"fe80::d615:8389:1288:7bed",&b.sin6_addr);b.sin6_port=htons(7000);int f=fcntl(s,F_GETFL,0);fcntl(s,F_SETFL,f|O_NONBLOCK);int r=connect(s,(void*)&b,sizeof(b));if(r<0&&errno!=EINPROGRESS){perror("connect");return 2;}struct pollfd p={s,POLLOUT,0};r=poll(&p,1,3000);int e=0;socklen_t n=sizeof(e);getsockopt(s,SOL_SOCKET,SO_ERROR,&e,&n);printf("poll=%d revents=%x error=%d %s\n",r,p.revents,e,strerror(e));return e?1:0;}
