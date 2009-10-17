#define _XOPEN_SOURCES 500
#include <features.h>
#define __USE_GNU
#include <dlfcn.h>
#undef __USE_GNU
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define INTBITS (sizeof(int) * 8)

#define OVERRIDE(name, ret, def_args)	\
  typedef ret libc_##name##_t def_args;	\
  static libc_##name##_t* orig_##name;	\
  ret name def_args

#define WRITES(fd, cstr) write(fd, (cstr), sizeof(cstr) - 1)

static int syncfds[1048576 / INTBITS];
static int usleep_val = 0;

static __inline void syncfd_set(int fd, int on)
{
  if (on)
    syncfds[fd / INTBITS] |= 1 << (fd % INTBITS);
  else
    syncfds[fd / INTBITS] &= ~ (1 << (fd % INTBITS));
}

static __inline int syncfd_isset(int fd)
{
  return (syncfds[fd / INTBITS] | (1 << fd % INTBITS)) != 0;
}

OVERRIDE(open, int, (const char* pathname, int flags, ...))
{
  int fd, osync_isset = (flags & O_SYNC) != 0, mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  if ((fd = (*orig_open)(pathname, flags & ~O_SYNC, mode)) != -1)
    syncfd_set(fd, osync_isset);
  return fd;
}

OVERRIDE(fsync, int, (int fd))
{
  return usleep_val != 0 ? (usleep(usleep_val), 0) : fsync(fd);
}

OVERRIDE(fdatasync, int, (int fd))
{
  return usleep_val != 0 ? (usleep(usleep_val), 0) : fdatasync(fd);
}

OVERRIDE(dup, int, (int oldfd))
{
  int newfd = (*orig_dup)(oldfd);
  if (newfd != -1)
    syncfd_set(newfd, syncfd_isset(oldfd));
  return newfd;
}

OVERRIDE(dup2, int, (int oldfd, int newfd))
{
  int ret = (*orig_dup2)(oldfd, newfd);
  if (ret != -1)
    syncfd_set(newfd, syncfd_isset(oldfd));
  return ret;
}

/* FIXME override fcntl */

OVERRIDE(write, ssize_t, (int fd, const void* buf, size_t count))
{
  ssize_t ret = (*orig_write)(fd, buf, count);
  if (ret > 0 && usleep_val != 0 && syncfd_isset(fd))
    usleep(usleep_val);
  return ret;
}

OVERRIDE(pwrite, ssize_t, (int fd, const void* buf, size_t count, off_t offset))
{
  ssize_t ret = (*orig_pwrite)(fd, buf, count, offset);
  if (ret > 0 && usleep_val != 0 && syncfd_isset(fd))
    usleep(usleep_val);
  return ret;
}

OVERRIDE(writev, ssize_t, (int fd, const struct iovec* iov, int iovcnt))
{
  ssize_t ret = (*orig_writev)(fd, iov, iovcnt);
  if (ret > 0 && usleep_val != 0 && syncfd_isset(fd))
    usleep(usleep_val);
  return ret;
}

__attribute__((constructor))
extern void fakeiops_init(void)
{
  const char* iopsstr;
  long iops;
  
  /* load symbols */
#define DLSYM(n) orig_##n = (libc_##n##_t*)dlsym(RTLD_NEXT, #n)
  DLSYM(open);
  DLSYM(fsync);
  DLSYM(fdatasync);
  DLSYM(dup);
  DLSYM(dup2);
  DLSYM(write);
  DLSYM(pwrite);
  DLSYM(writev);
#undef DLSYM
  
  /* determine microseconds to sleep */
  if ((iopsstr = getenv("FAKEIOPS")) == NULL) {
    WRITES(2, "fakeiops: $FAKEIOPS not set\n");
    return;
  }
  iops = strtol(iopsstr, NULL, 10);
  if (iops < 0 || iops == LONG_MAX) {
    WRITES(2, "fakeiops: invalid number specified in $FAKEIOPS\n");
    return;
  } else if (iops == 0) {
    return;
  }
  usleep_val = 1000000 / iops;
}
