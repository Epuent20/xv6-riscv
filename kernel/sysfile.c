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

  fs_mutation_acquire();
  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    fs_mutation_release();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    fs_mutation_release();
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    end_op();
    fs_mutation_release();
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
  fs_mutation_release();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  fs_mutation_release();
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

// Keep the directory topology stable with fs_mutation_acquire() during
// this walk. Hold at most one inode lock while following "..".
static int
renameancestor(struct inode *ancestor, struct inode *dir)
{
  if (ancestor->dev != dir->dev)
    return 0;

  struct inode *ip = idup(dir);
  while (ip != ancestor && ip->inum != ROOTINO) {
    ilock(ip);
    struct inode *parent = dirlookup(ip, "..", 0);
    iunlockput(ip);
    if (parent == 0)
      return 0;
    ip = parent;
  }
  int found = ip == ancestor;
  iput(ip);
  return found;
}

// Rewrite a dirent that is already backed by an allocated block.
static void
renamedirent(struct inode *dir, uint off, char *name, uint inum)
{
  struct dirent de = {0};
  if (name)
    strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if (writei(dir, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("rename: write dirent");
}

uint64
sys_rename(void)
{
  char old[MAXPATH], new[MAXPATH], oname[DIRSIZ], nname[DIRSIZ];
  struct inode *odp = 0, *ndp = 0, *ip = 0, *tp = 0;
  struct inode *first, *second;
  uint ooff, noff;
  int result = -1;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  fs_mutation_acquire();
  begin_op();
  odp = nameiparent(old, oname);
  ndp = nameiparent(new, nname);
  if (odp == 0 || ndp == 0 || odp->dev != ndp->dev ||
      namecmp(oname, ".") == 0 || namecmp(oname, "..") == 0 ||
      namecmp(nname, ".") == 0 || namecmp(nname, "..") == 0)
    goto done;

  // No namespace writer can change either entry or its offset while
  // the mutation lock is held, including during the ancestry walks.
  ilock(odp);
  ip = dirlookup(odp, oname, &ooff);
  iunlock(odp);
  if (ip == 0)
    goto done;

  ilock(ndp);
  tp = dirlookup(ndp, nname, &noff);
  iunlock(ndp);
  if (ip == tp) {
    result = 0;
    goto done;
  }

  // An inode in either parent's chain must never be locked as a child.
  // A regular file cannot be found by a walk over directory ancestors.
  if (renameancestor(ip, ndp) || (tp && renameancestor(tp, odp)))
    goto done;

  // Lock related parents ancestor first. Order unrelated ones by inum.
  first = odp;
  second = ndp;
  if (odp != ndp &&
      (renameancestor(ndp, odp) ||
       (ndp->inum < odp->inum && !renameancestor(odp, ndp)))) {
    first = ndp;
    second = odp;
  }

  ilock(first);
  if (second != first)
    ilock(second);
  ilock(ip);
  if (tp)
    ilock(tp);

  int isdir = ip->type == T_DIR;
  int cross = odp != ndp;
  if (odp->nlink == 0 || ndp->nlink == 0 ||
      (tp && (isdir != (tp->type == T_DIR) || (isdir && !isdirempty(tp)))) ||
      (isdir && cross && tp == 0 && ndp->nlink >= NLINK_MAX))
    goto unlock;

  // An insertion may allocate space. Finish it before removing old.
  if (tp == 0) {
    if (dirlink(ndp, nname, ip->inum) < 0)
      goto unlock;
  } else {
    renamedirent(ndp, noff, nname, ip->inum);
    tp->nlink--;
    iupdate(tp);
  }

  renamedirent(odp, ooff, 0, 0);
  if (isdir) {
    if (cross)
      renamedirent(ip, sizeof(struct dirent), "..", ndp->inum);
    if (cross || tp) {
      odp->nlink--;
      iupdate(odp);
    }
    if (cross && tp == 0) {
      ndp->nlink++;
      iupdate(ndp);
    }
  }
  result = 0;

unlock:
  if (tp)
    iunlock(tp);
  iunlock(ip);
  if (second != first)
    iunlock(second);
  iunlock(first);
done:
  if (tp)
    iput(tp);
  if (ip)
    iput(ip);
  if (ndp)
    iput(ndp);
  if (odp)
    iput(odp);
  end_op();
  fs_mutation_release();
  return result;
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

  fs_mutation_acquire();
  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    fs_mutation_release();
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

  end_op();
  fs_mutation_release();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  fs_mutation_release();
  return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
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

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd = -1, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  int mutating = (omode & (O_CREATE | O_TRUNC)) != 0;
  if (mutating)
    fs_mutation_acquire();
  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      goto done;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      goto done;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      goto done;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    goto done;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    goto done;
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

  //added
  f->append = (omode & O_APPEND) != 0;

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
done:
  end_op();
  if (mutating)
    fs_mutation_release();
  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  fs_mutation_acquire();
  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    fs_mutation_release();
    return -1;
  }
  iunlockput(ip);
  end_op();
  fs_mutation_release();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  fs_mutation_acquire();
  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    fs_mutation_release();
    return -1;
  }
  iunlockput(ip);
  end_op();
  fs_mutation_release();
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
