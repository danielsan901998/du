#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#define BUF_SIZE 4096
size_t parse_file(int dirfd, const char* name);
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

		for(int i=0;i<nread;){
			struct dirent* d = (struct dirent *)(buff+i);
			i+=d->d_reclen;
			if(strcmp(d->d_name,".")==0)
				continue;
			if(strcmp(d->d_name,"..")==0)
				continue;
			/* there is a possible race condition here as the file
			 * could be renamed between the readdir and the open */
			size+=parse_file(fd, d->d_name);
		}
	}
	return size;
}
struct statx statbuf;
size_t parse_file(int dirfd, const char* name){
	if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW,STATX_MODE | STATX_BLOCKS, &statbuf) != 0)
		return 0;
	size_t seconds = statbuf.stx_blocks*512;
	if(S_ISDIR(statbuf.stx_mode)){
		int fd = openat(dirfd,name,O_NOFOLLOW | O_NONBLOCK, O_RDONLY);
		if(fd==-1)
			perror("openat");
		else{
			seconds += folder_size(fd);
			close(fd);
		}
	}
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
		size_t size=parse_file(AT_FDCWD, filename);
		printf("%s\t%s\n",humanSize(size), filename);
	}
	return 0;
}
