#include <stdio.h>
#include <stdlib.h>

/* Import user configuration: */
#ifdef __Unikraft__
#include <uk/config.h>
#endif /* __Unikraft__ */

#if CONFIG_APPHELLOWORLD_SPINNER
#include <errno.h>
#include <time.h>

#include "monkey.h"

static void millisleep(unsigned int millisec) {
  struct timespec ts;
  int ret;

  ts.tv_sec = millisec / 1000;
  ts.tv_nsec = (millisec % 1000) * 1000000;
  do ret = nanosleep(&ts, &ts);
  while (ret && errno == EINTR);
}
#endif /* CONFIG_APPHELLOWORLD_SPINNER */

int main(int argc, char* argv[]) {
#if CONFIG_APPHELLOWORLD_PRINTARGS || CONFIG_APPHELLOWORLD_SPINNER
  int i;
#endif

  void* p = malloc(3 * 1024);
//   void* p = malloc(2 * 128);
  free(p);

//   void* ptrs[10];
//   for (int i = 0; i < 10; ++i) {
//     ptrs[i] = malloc(64);
//     printf("Alloc %d: addr = %p\n", i, ptrs[i]);
//   }

//   for (int i = 0; i < 10; ++i) {
//     free(ptrs[i]);
//   }

#if CONFIG_APPHELLOWORLD_PRINTARGS
  printf("Arguments: ");
  for (i = 0; i < argc; ++i) printf(" \"%s\"", argv[i]);
  printf("\n");
#endif /* CONFIG_APPHELLOWORLD_PRINTARGS */

#if CONFIG_APPHELLOWORLD_SPINNER
  i = 0;
  printf("\n\n\n");
  for (;;) {
    i %= (monkey3_frame_count * 3);
    printf("\r\033[2A %s \n", monkey3[i++]);
    printf(" %s \n", monkey3[i++]);
    printf(" %s ", monkey3[i++]);
    fflush(stdout);
    millisleep(250);
  }
#endif /* CONFIG_APPHELLOWORLD_SPINNER */

  return 0;
}
