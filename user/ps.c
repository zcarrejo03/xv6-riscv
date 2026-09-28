#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/pstat.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  struct pstat uproc[NPROC];
  int nprocs;
  int i;
  uint now;
  char *state;
  static char *states[] = {
    [USED]     "used",
    [SLEEPING] "sleeping",
    [RUNNABLE] "runnable",
    [RUNNING]  "running",
    [ZOMBIE]   "zombie"
  };

  nprocs = getprocs(uproc);
  if (nprocs < 0)
    exit(1);
  now = uptime();

  printf("pid\tstate\t\tsize\tage\tpriority\tppid\tname\n");
  for (i = 0; i < nprocs; i++) {
    state = states[uproc[i].state];
    printf("%d\t%s\t%ld\t", uproc[i].pid, state,
           (long)uproc[i].size);

    if (uproc[i].state == RUNNABLE)
      printf("%d", (int)(now - uproc[i].readytime));

    printf("\t%d\t%d\t%s\n", uproc[i].priority,
           uproc[i].ppid, uproc[i].name);
  }

  exit(0);
}
