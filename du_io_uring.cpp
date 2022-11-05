#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/resource.h>

#include <liburing.h>

#include <deque>

#define BUF_SIZE 16384*2

std::deque<int> queue;

static size_t size;
static struct io_uring ring;
static int sqes_in_flight=0;

static void drain_cqes()
{
    uint32_t head;
    struct io_uring_cqe *cqe;

    int count = 0;
    io_uring_for_each_cqe (&ring, head, cqe) {
        //TODO: handle error open file because of limit of open file descriptors
        if(cqe->res>0)
            queue.push_back(cqe->res);
        count++;
    }

    sqes_in_flight -= count;

    io_uring_cq_advance(&ring, count);
}

static struct io_uring_sqe *get_sqe()
{
    struct io_uring_sqe *sqe;

    sqe = io_uring_get_sqe(&ring);
    while (sqe == NULL) {
        int ret;

        drain_cqes();

        ret = io_uring_submit(&ring);
        if (ret < 0 && errno != EBUSY) {
            perror("io_uring_submit");
            exit(EXIT_FAILURE);
        }

        sqe = io_uring_get_sqe(&ring);
    }

    sqes_in_flight++;

    return sqe;
}

static void schedule_open(int parent_fd, const char* name)
{
    struct io_uring_sqe* sqe = get_sqe();
    io_uring_prep_openat(sqe, parent_fd, name, O_NOFOLLOW | O_NONBLOCK, O_RDONLY);
}
struct statx stat_buf[BUF_SIZE];
static void schedule_statx(int parent_fd, const char* name)
{
    struct io_uring_sqe* sqe = get_sqe();
    io_uring_prep_statx(sqe, parent_fd, name, AT_SYMLINK_NOFOLLOW,STATX_MODE | STATX_BLOCKS, &stat_buf[sqes_in_flight-1]);
}
static void parse_file(int dirfd, const char* name){
    struct statx statbuf;
    //printf("fd:%d\n",fd);
    //TODO: use AT_EMPTY_PATH on statx
    if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW,STATX_MODE | STATX_BLOCKS, &statbuf) != 0){
        printf("error statx %s\n",name);
        return;
    }
    if(S_ISDIR(statbuf.stx_mode)){
        size += statbuf.stx_blocks*512;
        schedule_open(dirfd,name);
    }
    else if(S_ISREG(statbuf.stx_mode))
        size += statbuf.stx_blocks*512;
    return;
}

static char buff[BUF_SIZE];
static void parse_directory(int fd){
    for (;;) {
        int nread = syscall(SYS_getdents64, fd, buff, BUF_SIZE);
        if (nread == -1){
            perror("getdents");
            break;
        }

        if (nread == 0)
            break;

        for(int i=0;i<nread;){
            struct dirent* d = (struct dirent *)(buff+i);
            i+=d->d_reclen;
            if(strcmp(d->d_name,".")==0)
                continue;
            if(strcmp(d->d_name,"..")==0)
                continue;
            schedule_statx(fd,d->d_name);
        }
        while (sqes_in_flight) {
            int ret = io_uring_submit_and_wait(&ring, sqes_in_flight);
            if (ret < 0 && errno != EBUSY) {
                perror("io_uring_submit_and_wait");
                exit(EXIT_FAILURE);
            }

            uint32_t head;
            struct io_uring_cqe *cqe;

            int count = 0;
            io_uring_for_each_cqe (&ring, head, cqe) {
                count++;
            }
            sqes_in_flight -= count;
            io_uring_cq_advance(&ring, count);
        }
        int count=0;
        for(int i=0;i<nread;){
            struct dirent* d = (struct dirent *)(buff+i);
            i+=d->d_reclen;
            if(strcmp(d->d_name,".")==0)
                continue;
            if(strcmp(d->d_name,"..")==0)
                continue;
            if(S_ISDIR(stat_buf[count].stx_mode)){
                size += stat_buf[count].stx_blocks*512;
                schedule_open(fd,d->d_name);
            }
            else if(S_ISREG(stat_buf[count].stx_mode))
                size += stat_buf[count].stx_blocks*512;
            count++;
        }
        while (sqes_in_flight) {
            int ret = io_uring_submit_and_wait(&ring, sqes_in_flight);
            if (ret < 0 && errno != EBUSY) {
                perror("io_uring_submit_and_wait");
                exit(EXIT_FAILURE);
            }

            drain_cqes();
        }
    }
}

static void submit_wait_until_complete(){
    while (sqes_in_flight) {
        int ret = io_uring_submit_and_wait(&ring, sqes_in_flight);
        if (ret < 0 && errno != EBUSY) {
            perror("io_uring_submit_and_wait");
            exit(EXIT_FAILURE);
        }

        drain_cqes();
        while(!queue.empty()){
            parse_directory(queue.front());
            queue.pop_front();
        }
    }
}

static const char *humanSize(size_t bytes)
{
    const char *suffix[] = {"B", "KB", "MB", "GB", "TB"};
    char length = sizeof(suffix) / sizeof(suffix[0]);

    int i = 0;
    double dblBytes = bytes;

    if (bytes > 1024)
        for (i = 0; (bytes / 1024) > 0 && i<length-1; i++, bytes /= 1024)
            dblBytes = bytes / 1024.0;

    static char output[200];
    sprintf(output, "%.01lf%s", dblBytes, suffix[i]);
    return output;
}

int main(int argc, char** argv){
    if(argc<2)
        return 0;
    if (io_uring_queue_init(BUF_SIZE, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        return 1;
    }
    struct rlimit rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
        perror("getrlimit");
        return 1;
    }

    if (geteuid() == 0 && rlim.rlim_max < 1048576)
        rlim.rlim_max = 1048576;

    if (rlim.rlim_cur < rlim.rlim_max) {
        rlim.rlim_cur = rlim.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rlim);
    }

    for(int i=1;i<argc;i++){
        size=0;
        char* filename=argv[i];
        size_t len = strlen(filename);

        parse_file(AT_FDCWD, filename);

        submit_wait_until_complete();
        printf("%s\t%s\n",humanSize(size), filename);
    }

    io_uring_queue_exit(&ring);
    return 0;
}
