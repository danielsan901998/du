#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#define BUF_SIZE 4196
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
size_t parse_fd(int fd);
size_t folder_size(int fd){
    size_t size=0;
    char buff[BUF_SIZE];


    for ( ; ; ) {
        int nread = syscall(SYS_getdents64, fd, buff, BUF_SIZE);
        if (nread == -1){
            perror("getdents");
            break;
        }

        if (nread == 0)
            break;

        //printf("nread: %d stat_size: %lu\n",nread, folder_size);
        for(int i=0;i<nread;){
            struct dirent* d = (struct dirent *)(buff+i);
            i+=d->d_reclen;
            if(strcmp(d->d_name,".")==0)
                continue;
            if(strcmp(d->d_name,"..")==0)
                continue;
            /* there is a possible race condition here as the file
             * could be renamed between the readdir and the open */
            int ffd;
            if ((ffd = openat(fd, d->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) == -1){
                if(errno!=ELOOP && errno!=ENXIO)
                    perror(d->d_name);
                continue;
            }
            size+=parse_fd(ffd);
        }
    }
    return size;
}
size_t parse_fd(int fd){
    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0)
        return 0;
    size_t seconds=0;
    if(S_ISDIR(statbuf.st_mode)){
        seconds += statbuf.st_blocks*512;
        //TODO: allocate buffer with st_size as length
        //  statbuf.st_size is the byte size of the directory
        //  use it to allocate enought memory to use as buffer
        //  for getdents syscall
        seconds += folder_size(fd);
    }
    else if(S_ISREG(statbuf.st_mode))
        seconds += statbuf.st_blocks*512;
    close(fd);
    return seconds;
}

int main(int argc, char** argv){
    if(argc<2)
        return 0;
    for(int i=1;i<argc;i++){
        char* filename=argv[i];
        size_t len = strlen(filename);
        if (filename[len -1] == '/')    // last character
            filename[len -1] = 0;
        size_t size=parse_fd(open(filename, O_RDONLY));
        printf("%s\t%s\n",humanSize(size), filename);
    }
    return 0;
}
