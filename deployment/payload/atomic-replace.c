#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_path(const char *operation, const char *path)
{
    (void)fprintf(stderr, "aula-atomic-replace: %s %s: %s\n", operation,
                  path, strerror(errno));
    return 1;
}

static int parent_directory(const char *path, char output[PATH_MAX])
{
    const char *slash;
    size_t length;

    if (path == NULL || path[0] != '/' || path[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    length = slash == path ? 1U : (size_t)(slash - path);
    if (length >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(output, path, length);
    output[length] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    char source_parent[PATH_MAX];
    char destination_parent[PATH_MAX];
    struct stat metadata;
    int directory_fd;

    if (argc != 3) {
        (void)fprintf(stderr,
                      "usage: aula-atomic-replace SOURCE_SYMLINK DESTINATION_SYMLINK\n");
        return 64;
    }
    if (parent_directory(argv[1], source_parent) != 0 ||
        parent_directory(argv[2], destination_parent) != 0) {
        return fail_path("rejecting invalid path", argv[1]);
    }
    if (strcmp(source_parent, destination_parent) != 0) {
        errno = EXDEV;
        return fail_path("requires a shared parent directory for", argv[1]);
    }
    if (lstat(argv[1], &metadata) != 0) {
        return fail_path("cannot inspect", argv[1]);
    }
    if (!S_ISLNK(metadata.st_mode)) {
        errno = EINVAL;
        return fail_path("source is not a symlink", argv[1]);
    }
    if (lstat(argv[2], &metadata) == 0) {
        if (!S_ISLNK(metadata.st_mode)) {
            errno = EEXIST;
            return fail_path("destination is not a symlink", argv[2]);
        }
    } else if (errno != ENOENT) {
        return fail_path("cannot inspect", argv[2]);
    }

    directory_fd = open(source_parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return fail_path("cannot open parent directory", source_parent);
    }
    if (rename(argv[1], argv[2]) != 0) {
        int saved_errno = errno;
        (void)close(directory_fd);
        errno = saved_errno;
        return fail_path("cannot replace", argv[2]);
    }
    if (fsync(directory_fd) != 0) {
        int saved_errno = errno;
        (void)close(directory_fd);
        errno = saved_errno;
        return fail_path("cannot persist parent directory", source_parent);
    }
    if (close(directory_fd) != 0) {
        return fail_path("cannot close parent directory", source_parent);
    }
    return 0;
}
