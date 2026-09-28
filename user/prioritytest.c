#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int pid;
  int before;

  printf("initial priority: %d\n", getpriority());

  if (setpriority(10) < 0) {
    printf("setpriority(10) failed\n");
    exit(1);
  }
  printf("parent priority after set: %d\n", getpriority());

  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    char *args[] = {"ps", 0};
    printf("child inherited priority: %d\n", getpriority());
    exec("ps", args);
    printf("exec ps failed\n");
    exit(1);
  }

  wait(0);
  before = getpriority();
  if (setpriority(50) != -1 || getpriority() != before) {
    printf("invalid priority test failed\n");
    exit(1);
  }
  printf("priority 50 rejected; current priority: %d\n", getpriority());
  exit(0);
}
