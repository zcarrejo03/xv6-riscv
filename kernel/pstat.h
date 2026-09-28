#ifndef PSTAT_H
#define PSTAT_H

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct pstat {
  int pid;
  enum procstate state;
  uint64 size;
  int ppid;
  char name[16];
};

#endif
