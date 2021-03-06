/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <fcntl.h>
#include <stdio.h>

int taskdebuglevel;
int taskcount;
int tasknswitch;
int taskexitval;
// 正在运行的任务
Task    *taskrunning;

// 需要被调度的上下文
Context taskschedcontext;
// 全局变量
Tasklist    taskrunqueue;

Task    **alltask;
// 整个task的数量
int     nalltask;

static char *argv0;
static  void        contextswitch(Context *from, Context *to);

static void
taskdebug(char *fmt, ...)
{
    va_list arg;
    char buf[128];
    Task *t;
    char *p;
    static int fd = -1;

return;
    va_start(arg, fmt);
    vfprint(1, fmt, arg);
    va_end(arg);
return;

    if(fd < 0){
        p = strrchr(argv0, '/');
        if(p)
            p++;
        else
            p = argv0;
        snprint(buf, sizeof buf, "/tmp/%s.tlog", p);
        if((fd = open(buf, O_CREAT|O_WRONLY, 0666)) < 0)
            fd = open("/dev/null", O_WRONLY);
    }

    va_start(arg, fmt);
    vsnprint(buf, sizeof buf, fmt, arg);
    va_end(arg);
    t = taskrunning;
    if(t)
        fprint(fd, "%d.%d: %s\n", getpid(), t->id, buf);
    else
        fprint(fd, "%d._: %s\n", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
    Task *t;
    ulong z;

    z = x<<16;  /* hide undefined 32-bit shift from 32-bit compilers */
    z <<= 16;
    z |= y;
    t = (Task*)z;

//print("taskstart %p\n", t);
    t->startfn(t->startarg);
//print("taskexits %p\n", t);
    taskexit(0);
//print("not reacehd\n");
}

static int taskidgen;

static Task*
taskalloc(void (*fn)(void*), void *arg, uint stack)
{
    Task *t;
    sigset_t zero;
    uint x, y;
    ulong z;

    /* allocate the task and stack together */
    t = malloc(sizeof *t+stack);
    if(t == nil){
        fprint(2, "taskalloc malloc: %r\n");
        abort();
    }
    memset(t, 0, sizeof *t);
    // 注意这里的 + 1操作，意味着指针的偏移量，是多少
    t->stk = (uchar*)(t+1);
    t->stksize = stack;
    t->id = ++taskidgen;
    t->startfn = fn;
    t->startarg = arg;

    /* do a reasonable initialization */
    memset(&t->context.uc, 0, sizeof t->context.uc);
    // 初始化信号量
    sigemptyset(&zero);
    // 设置为阻塞模式
    sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);

    /* must initialize with current context */
    if(getcontext(&t->context.uc) < 0){
        fprint(2, "getcontext: %r\n");
        abort();
    }

    /* call makecontext to do the real work. */
    /* leave a few words open on both ends */
    // 初始化栈指针
    t->context.uc.uc_stack.ss_sp = t->stk+8;
    // 初始化栈空间
    t->context.uc.uc_stack.ss_size = t->stksize-64;
#if defined(__sun__) && !defined(__MAKECONTEXT_V2_SOURCE)       /* sigh */
#warning "doing sun thing"
    /* can avoid this with __MAKECONTEXT_V2_SOURCE but only on SunOS 5.9 */
    t->context.uc.uc_stack.ss_sp =
        (char*)t->context.uc.uc_stack.ss_sp
        +t->context.uc.uc_stack.ss_size;
#endif
    /*
     * All this magic is because you have to pass makecontext a
     * function that takes some number of word-sized variables,
     * and on 64-bit machines pointers are bigger than words.
     */
//print("make %p\n", t);
    z = (ulong)t;
    y = z;
    z >>= 16;   /* hide undefined 32-bit shift from 32-bit compilers */
    x = z>>16;
    // 创建上下文，并保存到uc中
    makecontext(&t->context.uc, (void(*)())taskstart, 2, y, x);

    return t;
}

int
taskcreate(void (*fn)(void*), void *arg, uint stack)
{
    int id;
    Task *t;

    // 创建一个任务
    t = taskalloc(fn, arg, stack);
    // coroutine++
    taskcount++;
    id = t->id;

    // task链的初始化
    if (nalltask%64 == 0) {
        alltask = realloc(alltask, (nalltask+64)*sizeof(alltask[0]));
        if(alltask == nil){
            fprint(2, "out of memory\n");
            abort();
        }
    }
    t->alltaskslot = nalltask;
    // 在queue中保存这个task
    alltask[nalltask++] = t;
    // 放到queue中
    taskready(t);
    return id;
}

void
tasksystem(void)
{
    // 如果没有设置成系统任务
    if(!taskrunning->system){
        taskrunning->system = 1;
        --taskcount;
    }
}

void
taskswitch(void)
{
    // 获取栈空间
    needstack(0);
    // 执行上下文切换
    contextswitch(&taskrunning->context, &taskschedcontext);
}

void
taskready(Task *t)
{
    // 准备好了
    t->ready = 1;
    // 加到队列尾巴上
    addtask(&taskrunqueue, t);
}

int
taskyield(void)
{
    int n;

    // 备份之前
    n = tasknswitch;
    // 放到等待列表中
    taskready(taskrunning);
    taskstate("yield");
    // 执行上下文切换
    taskswitch();
    // 自己等待期间运行的任务数
    return tasknswitch - n - 1;
}

int
anyready(void)
{
    return taskrunqueue.head != nil;
}

// 直接退出进程
void
taskexitall(int val)
{
    exit(val);
}

void
taskexit(int val)
{
    taskexitval = val;
    taskrunning->exiting = 1;
    taskswitch();
}

static void
contextswitch(Context *from, Context *to)
{
    // 直接使用swapcontext来执行上下文切换
    // 保存当前上下文到from，并执行to上下文
    // 当执行完毕后，跳转到from上下文中
    if(swapcontext(&from->uc, &to->uc) < 0){
        fprint(2, "swapcontext failed: %r\n");
        assert(0);
    }
}

// 这个整个调度器的入口
static void
taskscheduler(void)
{
    int i;
    Task *t;

    taskdebug("scheduler enter");
    // 调度程序一直从任务列表中挑选任务
    for(;;){
        // 如果没有任务，直接进程退出
        if(taskcount == 0)
            exit(taskexitval);
        // 获取队列头
        t = taskrunqueue.head;
        if(t == nil){
            fprint(2, "no runnable tasks! %d tasks stalled\n", taskcount);
            exit(1);
        }
        // 删除该任务
        deltask(&taskrunqueue, t);
        t->ready = 0;
        // 更新当前任务列表
        taskrunning = t;
        tasknswitch++;
        taskdebug("run %d (%s)", t->id, t->name);
        // 切到新的context
        // 这里的taskschedcontext，是目前被调度的context
        // 换成新的coroutine的context
        contextswitch(&taskschedcontext, &t->context);
//print("back in scheduler\n");
        taskrunning = nil;
        // 进行资源释放
        if(t->exiting){
            // 减少当前任务列表
            if(!t->system)
                // 如果不是系统任务，那么直接将任务数-1
                taskcount--;
            i = t->alltaskslot;
            alltask[i] = alltask[--nalltask];
            alltask[i]->alltaskslot = i;
            free(t);
        }
    }
}

void**
taskdata(void)
{
    return &taskrunning->udata;
}

/*
 * debugging
 */
void
taskname(char *fmt, ...)
{
    va_list arg;
    Task *t;

    t = taskrunning;
    va_start(arg, fmt);
    vsnprint(t->name, sizeof t->name, fmt, arg);
    va_end(arg);
}

char*
taskgetname(void)
{
    return taskrunning->name;
}

void
taskstate(char *fmt, ...)
{
    va_list arg;
    Task *t;

    // 获取当前的任务列表
    t = taskrunning;
    va_start(arg, fmt);
    // 将状态描述写到改状态中
    vsnprint(t->state, sizeof t->name, fmt, arg);
    va_end(arg);
}

char*
taskgetstate(void)
{
    // 获取当前任务的状态
    return taskrunning->state;
}

void
needstack(int n)
{
    Task *t;

    t = taskrunning;

    // 栈空间太小
    if((char*)&t <= (char*)t->stk
    || (char*)&t - (char*)t->stk < 256+n){
        fprint(2, "task stack overflow: &t=%p tstk=%p n=%d\n", &t, t->stk, 256+n);
        abort();
    }
}

// 打印全局的coroutine列表
static void
taskinfo(int s)
{
    int i;
    Task *t;
    char *extra;

    fprint(2, "task list:\n");
    for(i=0; i<nalltask; i++){
        t = alltask[i];
        if(t == taskrunning)
            extra = " (running)";
        else if(t->ready)
            extra = " (ready)";
        else
            extra = "";
        fprint(2, "%6d%c %-20s %s%s\n",
            t->id, t->system ? 's' : ' ',
            t->name, t->state, extra);
    }
}

/*
 * startup
 */

static int taskargc;
static char **taskargv;
int mainstacksize;

static void
taskmainstart(void *v)
{
    taskname("taskmain");
    taskmain(taskargc, taskargv);
}

// 直接在这个lib中就有一个main函数
int
main(int argc, char **argv)
{
    struct sigaction sa, osa;

    // 信号处理函数
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = taskinfo;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGQUIT, &sa, &osa);

#ifdef SIGINFO
    sigaction(SIGINFO, &sa, &osa);
#endif

    argv0 = argv[0];
    // 都是全局变量
    taskargc = argc;
    taskargv = argv;

    // main coroutine
    if(mainstacksize == 0)
        mainstacksize = 256*1024;
    // 创建main任务
    // 25k的栈空间
    taskcreate(taskmainstart, nil, mainstacksize);
    // 开始执行调度
    taskscheduler();
    // 不可能出现
    fprint(2, "taskscheduler returned in main!\n");
    abort();
    return 0;
}

/*
 * hooray for linked lists
 */
void
addtask(Tasklist *l, Task *t)
{
    if(l->tail){
        l->tail->next = t;
        t->prev = l->tail;
    }else{
        l->head = t;
        t->prev = nil;
    }
    l->tail = t;
    t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
    if(t->prev)
        t->prev->next = t->next;
    else
        l->head = t->next;
    if(t->next)
        t->next->prev = t->prev;
    else
        l->tail = t->prev;
}

unsigned int
taskid(void)
{
    return taskrunning->id;
}

