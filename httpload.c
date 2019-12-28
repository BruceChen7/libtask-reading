#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <task.h>
#include <stdlib.h>

enum
{
    STACK = 32768
};

char *server;
char *url;

void fetchtask(void*);

void
taskmain(int argc, char **argv)
{
    int i, n;

    // 参数数量不对
    if(argc != 4){
        fprintf(stderr, "usage: httpload n server url\n");
        taskexitall(1);
    }
    // 负载数
    n = atoi(argv[1]);
    // 获取server地址
    server = argv[2];
    // 获取url
    url = argv[3];

    for(i=0; i<n; i++){
        // 同时创建n个任务
        taskcreate(fetchtask, 0, STACK);
        while(taskyield() > 1)
            ;
        sleep(1);
    }
}

void
fetchtask(void *v)
{
    int fd, n;
    char buf[512];

    fprintf(stderr, "starting...\n");
    for(;;){
        if((fd = netdial(TCP, server, 80)) < 0){
            fprintf(stderr, "dial %s: %s (%s)\n", server, strerror(errno), taskgetstate());
            continue;
        }
        snprintf(buf, sizeof buf, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", url, server);
        fdwrite(fd, buf, strlen(buf));
        while((n = fdread(fd, buf, sizeof buf)) > 0)
            ;
        close(fd);
        write(1, ".", 1);
    }
}
