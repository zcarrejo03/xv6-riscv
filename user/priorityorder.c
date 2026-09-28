#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int ready[2], gate[2];
  int pid;
  char signal;

  if (pipe(ready) < 0 || pipe(gate) < 0 || setpriority(49) < 0) {
    printf("priorityorder: setup failed\n");
    exit(1);
  }

  for (int i = 0; i < 2; i++) {
    pid = fork();
    if (pid < 0) {
      printf("priorityorder: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      int level = (i == 0) ? 5 : 20;
      close(ready[0]);
      close(gate[1]);

      if (setpriority(level) < 0) {
        printf("priorityorder: setpriority failed\n");
        exit(1);
      }
      signal = 'R';
      if (write(ready[1], &signal, 1) != 1 ||
          read(gate[0], &signal, 1) != 1) {
        printf("priorityorder: pipe failed\n");
        exit(1);
      }

      printf("priority %d started\n", getpriority());
      for (volatile int work = 0; work < 5000000; work++)
        ;
      printf("priority %d finished\n", getpriority());
      exit(0);
    }
  }

  close(ready[1]);
  close(gate[0]);
  for (int i = 0; i < 2; i++) {
    if (read(ready[0], &signal, 1) != 1) {
      printf("priorityorder: ready signal failed\n");
      exit(1);
    }
  }

  printf("both children ready; releasing priority 5 and 20\n");
  if (write(gate[1], "GG", 2) != 2) {
    printf("priorityorder: release failed\n");
    exit(1);
  }

  wait(0);
  wait(0);
  printf("both children finished\n");
  exit(0);
}
