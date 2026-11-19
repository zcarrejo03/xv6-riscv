//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // dp may have been unlinked while we resolved it; linking into an
  // orphaned directory leaks ip (itrunc discards the record without
  // dropping ip->nlink).  create() has the same guard.
  if (dp->nlink == 0) {
    iunlockput(dp);
    goto bad;
  }
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  acquiresleep(&namespace_lock);
  if ((dp = nameiparent(path, name)) == 0) {
    releasesleep(&namespace_lock);
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  releasesleep(&namespace_lock);
  end_op();

  return 0;

bad:
  iunlockput(dp);
  releasesleep(&namespace_lock);
  end_op();
  return -1;
}

static struct inode *
create_locked(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip;

  // Callers have already started a log transaction.
  acquiresleep(&namespace_lock);
  ip = create_locked(path, type, major, minor);
  releasesleep(&namespace_lock);
  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  f->append = (omode & O_APPEND) != 0;

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, p->sz, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// Return 1 if dir is ancestor or lies below it, 0 otherwise,
// and -1 if the parent chain cannot be followed.
// Caller holds namespace_lock and no inode locks.
static int
rename_descendant(struct inode *dir, struct inode *ancestor)
{
  struct inode *ip = idup(dir);
  struct inode *parent;

  for (;;) {
    if (ip == ancestor) {
      iput(ip);
      return 1;
    }

    ilock(ip);
    if (ip->type != T_DIR || ip->nlink == 0) {
      iunlockput(ip);
      return -1;
    }

    if (ip->inum == ROOTINO) {
      iunlockput(ip);
      return 0;
    }

    parent = dirlookup(ip, "..", 0);
    iunlockput(ip);
    if (parent == 0)
      return -1;
    ip = parent;
  }
}

static int rename_locked(char *old, char *new);

uint64
sys_rename(void)
{
  char old[MAXPATH], new[MAXPATH];
  int result;

  if (argstr(0, old, MAXPATH) < 0 ||
      argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  acquiresleep(&namespace_lock);
  result = rename_locked(old, new);
  releasesleep(&namespace_lock);
  end_op();

  return result;
}

// Caller holds namespace_lock inside a log transaction.
static int
rename_locked(char *old, char *new)
{
  struct inode *op = 0, *np = 0, *ip = 0, *target = 0;
  char oldname[DIRSIZ], newname[DIRSIZ];
  struct dirent de, dotdot;
  uint oldoff = 0, newoff = 0;
  int isdir, moving, delta;
  int result = -1;

  op = nameiparent(old, oldname);
  np = nameiparent(new, newname);
  if (op == 0 || np == 0)
    goto out;

  if (namecmp(oldname, ".") == 0 ||
      namecmp(oldname, "..") == 0 ||
      namecmp(newname, ".") == 0 ||
      namecmp(newname, "..") == 0)
    goto out;

  if (op->dev != np->dev)
    goto out;

  ilock(op);
  if (op->type == T_DIR && op->nlink > 0)
    ip = dirlookup(op, oldname, &oldoff);
  iunlock(op);
  if (ip == 0 || ip == op || ip == np)
    goto out;

  ilock(ip);
  isdir = ip->type == T_DIR;
  iunlock(ip);

  // Walk before locking parents: the walk may visit either parent.
  if (isdir && rename_descendant(np, ip) != 0)
    goto out;

  // The namespace lock excludes competing create/unlink/rename
  // operations. Lock distinct parents once, in inode-number order.
  if (op == np) {
    ilock(op);
  } else if (op->inum < np->inum) {
    ilock(op);
    ilock(np);
  } else {
    ilock(np);
    ilock(op);
  }

  if (op->type != T_DIR || np->type != T_DIR ||
      op->nlink == 0 || np->nlink == 0)
    goto parents;

  target = dirlookup(np, newname, &newoff);

  // Identical names or hard links to the same inode are a no-op.
  if (target == ip) {
    result = 0;
    goto parents;
  }

  // Avoid locking a parent again as a child.
  if (target == op || target == np)
    goto parents;

  ilock(ip);
  if (target)
    ilock(target);

  if (ip->nlink < 1)
    goto children;

  if (target) {
    if (target->nlink < 1 ||
        (isdir != (target->type == T_DIR)))
      goto children;
    if (target->type == T_DIR && !isdirempty(target))
      goto children;
  }

  moving = isdir && op != np;
  delta = moving - (target && target->type == T_DIR);

  if (moving && op->nlink < 2)
    goto children;
  if (delta > 0 && np->nlink >= NLINK_MAX)
    goto children;

  // Validate ".." before making any changes.
  if (moving) {
    if (readi(ip, 0, (uint64)&dotdot, sizeof(de),
              sizeof(dotdot)) != sizeof(dotdot) ||
        namecmp(dotdot.name, "..") != 0 ||
        dotdot.inum != op->inum)
      goto children;
  }

  memset(&de, 0, sizeof(de));
  de.inum = ip->inum;
  strncpy(de.name, newname, DIRSIZ);

  // Renaming within one directory to an unused name needs no
  // allocation, even when that directory or the disk is full.
  if (op == np && target == 0) {
    if (writei(op, 0, (uint64)&de, oldoff, sizeof(de)) != sizeof(de))
      panic("rename: same directory");
    result = 0;
    goto children;
  }

  // Create/replace the destination first. For an absent destination,
  // dirlink may fail to allocate; the source is still untouched.
  if (target) {
    if (writei(np, 0, (uint64)&de, newoff, sizeof(de)) != sizeof(de))
      panic("rename: destination");
  } else {
    if (dirlink(np, newname, ip->inum) < 0)
      goto children;
  }

  // All remaining writes update existing, allocated directory entries
  // using kernel buffers, so no further allocation is needed.
  memset(&de, 0, sizeof(de));
  if (writei(op, 0, (uint64)&de, oldoff, sizeof(de)) != sizeof(de))
    panic("rename: source");

  if (moving) {
    dotdot.inum = np->inum;
    if (writei(ip, 0, (uint64)&dotdot, sizeof(de),
               sizeof(dotdot)) != sizeof(dotdot))
      panic("rename: dotdot");
    op->nlink--;
    iupdate(op);
  }

  if (delta != 0) {
    np->nlink += delta;
    iupdate(np);
  }

  if (target) {
    target->nlink--;
    iupdate(target);
  }

  result = 0;

children:
  if (target)
    iunlock(target);
  iunlock(ip);

parents:
  if (op != np)
    iunlock(np);
  iunlock(op);

out:
  // Drop references after releasing all inode locks. iput() may
  // reclaim a replaced inode whose final link was removed.
  if (target)
    iput(target);
  if (ip)
    iput(ip);
  if (np)
    iput(np);
  if (op)
    iput(op);
  return result;
}
