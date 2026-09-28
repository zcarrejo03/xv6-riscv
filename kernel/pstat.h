#ifndef PSTAT_H
#define PSTAT_H

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct pstat {
  int pid;
  int priority;
  enum procstate state;
  uint64 size;
  int ppid;
  char name[16];
};

#endif
