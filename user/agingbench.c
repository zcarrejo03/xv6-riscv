#include "kernel/types.h"
#include "user/user.h"

struct result {
  int priority;
  uint response;
  uint turnaround;
};

int
main(void)
{
  int ready[2], gate[2], results[2];
  struct result r[2];
  char signal;
  uint released;

  if (pipe(ready) < 0 || pipe(gate) < 0 ||
      pipe(results) < 0 || setpriority(49) < 0) {
    printf("agingbench: setup failed\n");
    exit(1);
  }

  for (int i = 0; i < 2; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("agingbench: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      int level = (i == 0) ? 0 : 20;
      uint start;
      volatile uint work;
      uint limit = (level == 20) ? 2400000000U : 5000000U;
      struct result mine;

      close(ready[0]);
      close(gate[1]);
      close(results[0]);
      if (setpriority(level) < 0)
        exit(1);

      signal = 'R';
      if (write(ready[1], &signal, 1) != 1 ||
          read(gate[0], &released, sizeof(released)) != sizeof(released))
        exit(1);

      start = uptime();
      for (work = 0; work < limit; work++)
        ;

      mine.priority = level;
      mine.response = start - released;
      mine.turnaround = uptime() - released;
      if (write(results[1], &mine, sizeof(mine)) != sizeof(mine))
        exit(1);
      exit(0);
    }
  }

  close(ready[1]);
  close(gate[0]);
  close(results[1]);

  for (int i = 0; i < 2; i++)
    if (read(ready[0], &signal, 1) != 1)
      exit(1);

  released = uptime();
  for (int i = 0; i < 2; i++)
    if (write(gate[1], &released, sizeof(released)) != sizeof(released))
      exit(1);

  for (int i = 0; i < 2; i++)
    if (read(results[0], &r[i], sizeof(r[i])) != sizeof(r[i]))
      exit(1);

  wait(0);
  wait(0);
  for (int i = 0; i < 2; i++)
    printf("priority %d: response %d, turnaround %d ticks\n",
           r[i].priority, (int)r[i].response, (int)r[i].turnaround);

  int response_sum = r[0].response + r[1].response;
  int turnaround_sum = r[0].turnaround + r[1].turnaround;
  printf("average response: %d.%d ticks\n",
         response_sum / 2, (response_sum % 2) * 5);
  printf("average turnaround: %d.%d ticks\n",
         turnaround_sum / 2, (turnaround_sum % 2) * 5);
  exit(0);
}
