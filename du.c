#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
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
    size_t seconds=0;
    DIR *d;
    struct dirent *dp;
    int ffd;


    if ((d = fdopendir(fd)) == NULL) {
        close(fd);
        return seconds;
    }
    while ((dp = readdir(d)) != NULL) {
        if(strcmp(dp->d_name,".")==0)
            continue;
        if(strcmp(dp->d_name,"..")==0)
            continue;
        /* there is a possible race condition here as the file
         * could be renamed between the readdir and the open */
        if ((ffd = openat(fd, dp->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) == -1){
            if(errno!=ELOOP)
                perror(dp->d_name);
            continue;
        }
        seconds+=parse_fd(ffd);
        close(ffd);
    }
    closedir(d); // note this implicitly closes dfd
    return seconds;
}
size_t parse_fd(int fd){
    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0)
        return 0;
    size_t seconds=0;
    if(S_ISDIR(statbuf.st_mode)){
        seconds += statbuf.st_blocks*512;
        seconds += folder_size(fd);
    }
    else if(S_ISREG(statbuf.st_mode))
        seconds += statbuf.st_blocks*512;
    return seconds;
}

int main(int argc, char** argv){
    if(argc<2)
        return 0;
    for(int i=1;i<argc;i++){
        char* filename=argv[i];
        size_t len = strlen(filename);
        if (filename[len -1] == '/')    // one character
            filename[len -1] = 0;    // one character
        size_t size=parse_fd(open(filename, O_RDONLY));
        printf("%s\t%s\n",humanSize(size), filename);
    }
    return 0;
}
