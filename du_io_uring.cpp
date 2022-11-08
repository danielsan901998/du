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
#include <string>
#include <memory>

#define BUF_SIZE 16384

struct file{
    enum class State {OPEN, STAT};
    int parent_fd;
    int fd = -1;
    int ret;
    std::string name;
    State state;
    struct statx stat;
};
std::deque<file*> queue;

static size_t size;
static struct io_uring ring;
static int sqes_in_flight=0;

static void drain_cqes()
{
    uint32_t head;
    struct io_uring_cqe *cqe;

    int count = 0;
    io_uring_for_each_cqe (&ring, head, cqe) {
        file* f = (file*)io_uring_cqe_get_data(cqe);
        f->ret=cqe->res;
        queue.push_back(f);
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

static void schedule_open(file* f)
{
    f->state=file::State::OPEN;
    struct io_uring_sqe* sqe = get_sqe();
    io_uring_prep_openat(sqe, f->parent_fd, f->name.c_str(), O_NOFOLLOW | O_NONBLOCK, O_RDONLY);
    io_uring_sqe_set_data(sqe, f);
}
static void schedule_statx(file* f)
{
    f->state=file::State::STAT;
    struct io_uring_sqe* sqe = get_sqe();
    io_uring_prep_statx(sqe, f->parent_fd, f->name.c_str(), AT_SYMLINK_NOFOLLOW,STATX_MODE | STATX_BLOCKS, &f->stat);
    io_uring_sqe_set_data(sqe, f);
}

static char buff[BUF_SIZE];
static void parse_directory(int fd){
    for (;;) {
        int nread = syscall(SYS_getdents64, fd, buff, BUF_SIZE);
        if (nread == -1){
            perror("getdents");
            fprintf(stderr, "(%d): %s\n",fd, strerror(errno));
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
            file* f = new file;
            f->parent_fd=fd;
            f->name=d->d_name;
            schedule_statx(f);
        }
    }
}

static void submit_wait_until_complete(){
    while (sqes_in_flight) {
        int ret = io_uring_submit_and_wait(&ring, 1);
        if (ret < 0 && errno != EBUSY) {
            perror("io_uring_submit_and_wait");
            exit(EXIT_FAILURE);
        }

        drain_cqes();
        while(!queue.empty()){
            file* f = queue.front();
            queue.pop_front();
            if(f->state==file::State::STAT){
                if(f->ret==0){
                    size += f->stat.stx_blocks*512;
                    if(S_ISDIR(f->stat.stx_mode))
                        schedule_open(f);
                }
            }else{
                if(f->ret<0){
                    fprintf(stderr, "error opening ");
                    fprintf(stderr, ": %s\n", strerror(-ret));
                    continue;
                }
                f->fd=f->ret;
                parse_directory(f->fd);
            }
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

        file* f = new file;
        f->parent_fd=AT_FDCWD;
        f->name=filename;
        schedule_statx(f);

        submit_wait_until_complete();
        printf("%s\t%s\n",humanSize(size), filename);
    }

    io_uring_queue_exit(&ring);
    return 0;
}
