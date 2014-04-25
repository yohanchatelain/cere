#define _LARGEFILE64_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <malloc.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "dump.h"
#include "counters.h"
#include "err.h"
#include "mallochook.h"

#define _DEBUG 1
#undef _DEBUG

static const char dump_prefix[] = "dump";
static const char pagelog_suffix[] = "hotpages.map";
static const char core_suffix[] = "core.map";

struct dump_state state __attribute__ ((aligned (PAGESIZE)));
static void
mru_handler(int sig, siginfo_t *si, void *unused)
{
  char * touched_addr = si->si_addr;
  char * start_of_page = round_to_page(touched_addr);

#ifdef _DEBUG
  printf("Detected access at: %p -- Start of Page: %p\n",
         touched_addr, start_of_page);
#endif

  /* Unprotect Page */
  int result = mprotect((void*)start_of_page, PAGESIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
  assert(result != -1);


  /* Add page to page cache */
  /* we need to evict one of the pages, reprotect it ! */
  if (state.pages_cache[state.last_page] != 0) {
#ifdef _DEBUG
      printf("Reprotecting page %p\n", state.pages_cache[state.last_page]);
#endif
      int result = mprotect((void*)state.pages_cache[state.last_page], PAGESIZE, PROT_NONE);
      assert(result != -1);
  }

  state.pages_cache[state.last_page] = start_of_page;
  state.last_page = (state.last_page + 1)%state.log_size;

#ifdef _DEBUG
  printf("page cache [ ");
  for (int i = 0; i <state.log_size; i++) {
      int c = (i + state.last_page) % state.log_size;
      printf("%p ", state.pages_cache[c]);
  }
  printf(" ]\n");
#endif
}

static char
hex(char value)
{
  switch(value) {
    case 0: return '0';
    case 1: return '1';
    case 2: return '2';
    case 3: return '3';
    case 4: return '4';
    case 5: return '5';
    case 6: return '6';
    case 7: return '7';
    case 8: return '8';
    case 9: return '9';
    case 10: return 'a';
    case 11: return 'b';
    case 12: return 'c';
    case 13: return 'd';
    case 14: return 'e';
    case 15: return 'f';
  }
  assert(0);
}

static
void fill_dump_name(char buf[], int stack_pos, off64_t addr)
{
    //snprintf(buf, sizeof(buf), "%s/%lx.memdump", state.dump_path, start);
    const char suffix[] = ".memdump";
    int digits = MAX_DIGITS;

    size_t i, j;
    for (i = 0; state.dump_path[stack_pos][i] != '\0'; i++)
      buf[i] = state.dump_path[stack_pos][i];
    buf[i++] = '/';

    while(digits--) {
        buf[i+digits] = hex(addr % 16);
        addr = addr / 16;
    }
    i += MAX_DIGITS;

    for (j = 0; suffix[j] != '\0'; j++)
      buf[i++] = suffix[j];

    buf[i++] = '\0';
}

/* dump_region: dumps to a file "<start>.memdump"
 * the memory segment starting at address <start>
 * and ending at address <end>
 */
static
bool write_pages(char path[], off64_t start, off64_t stop)
{
    char buf[PAGESIZE];
    int out = open(path, O_WRONLY|O_CREAT|O_EXCL,S_IRWXU);

    if (out <= 0) {
        /* region already dumped */
        return false;
        //Should check errno but it is protected
        //if (errno == EEXIST) {
          //fprintf(stderr, "already exists %s\n", path);
        //  return false;
        //}
        //else
        //errx(EXIT_FAILURE, "could not dump %s\n", path);
    }

    lseek64(state.mem_fd, start, SEEK_SET);

    while(start < stop) {
        int rd = read(state.mem_fd, buf, PAGESIZE);
        int wr = write(out, buf, rd);
        assert(wr == PAGESIZE && rd == PAGESIZE);
        start += PAGESIZE;
    }

    close(out);
    return true;
}

static
void dump_pages(off64_t start, off64_t stop)
{
  /* Dump Page */
  char current_path[MAX_PATH];
  fill_dump_name(current_path, state.stack_pos, start);
  if (write_pages(current_path, start, stop)) {
      /* Link in parent's dumps */
      for (int i=0; i < state.stack_pos; i++) {
          char parent_path[MAX_PATH];
          fill_dump_name(parent_path, i, start);
          link(current_path, parent_path);
      }
  }
}

static void
dump_handler(int sig, siginfo_t *si, void *unused)
{

  mtrace_deactivate();
  char * touched_addr = si->si_addr;
  char * start_of_page = round_to_page(touched_addr);

  /* Unprotect Page */
   int result = mprotect((char*)start_of_page, PAGESIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
   assert(result != -1);

#ifdef _DEBUG
  printf("DUMP Detected access at: %p -- Start of Page: %p\n",
         touched_addr, start_of_page);
#endif

  /* Dump page */
  dump_pages((off64_t)start_of_page, (off64_t)start_of_page+PAGESIZE);

  /* Do the mru logging */
  //mru_handler(sig, si, unused);
  mtrace_activate();
}

static void
ignore_handler(int sig, siginfo_t *si, void *unused)
{
  char * touched_addr = si->si_addr;
  char * start_of_page = round_to_page(touched_addr);

  /* Unprotect Page */
  int result = mprotect((void*)start_of_page, PAGESIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
  assert(result != -1);

#ifdef _DEBUG
  printf("IGN Detected access at: %p -- Start of Page: %p\n",
         touched_addr, start_of_page);
#endif
}


static void
page_log_dump(void)
{
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/%s", state.dump_path[state.stack_pos], pagelog_suffix);
  FILE * f = fopen(path, "w");
  for (int i = 0; i <state.log_size; i++) {
      int c = (i + state.last_page) % state.log_size;
      fprintf(f, "%lx\n", (off64_t)state.pages_cache[c]);
  }
  fclose(f);
}

static void
set_ignore(void)
{
#ifdef _DEBUG
  printf("set_ignore()\n");
#endif
  state.dump_sa = IGNORE_SA;
  state.sa.sa_sigaction = ignore_handler;
  int res = sigaction(SIGSEGV, &state.sa, NULL);
  assert(res != -1);
}

static void
set_dump(void)
{
#ifdef _DEBUG
  printf("set_dump()\n");
#endif
  state.dump_sa = DUMP_SA;
  state.sa.sa_sigaction = dump_handler;
  int res = sigaction(SIGSEGV, &state.sa, NULL);
  assert(res != -1);
}

static void
set_mru(void)
{
#ifdef _DEBUG
  printf("set_mru()\n");
#endif
  state.dump_sa = MRU_SA;
  state.sa.sa_sigaction = mru_handler;
  int res = sigaction(SIGSEGV, &state.sa, NULL);
  assert(res != -1);
}

static void
open_mem_fd(void)
{
  char path[MAX_PATH];
  pid_t pid = getpid();
  snprintf(path, sizeof(path), "/proc/%d/mem", pid);
  state.mem_fd = open(path, O_RDONLY);
  if(!state.mem_fd == -1)
      errx(EXIT_FAILURE, "Error opening /proc/%d/mem interface", pid);
}

static
void configure_sigaction(void)
{
  /* Configure an alternative stack for
     the signal handler */
  stack_t ss;
  ss.ss_sp = state.hs;
  ss.ss_size = sizeof(state.hs);
  ss.ss_flags = 0;
  int res = sigaltstack(&ss, NULL);
  assert(res != -1);

  /* Configure the ignore signal handler */
  state.sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // | SA_NODEFER;
  sigemptyset(&state.sa.sa_mask);
  set_ignore();
}


static void
unlock_mem(void)
{
  char *p;
  char path[MAX_PATH];

  pid_t my_pid = getpid();
  snprintf(path, sizeof(path), "/proc/%d/maps", my_pid);
  FILE * maps = fopen(path, "r");

  if(!maps)
    errx(EXIT_FAILURE, "Error reading the memory using /proc/ interface");

  char buf[BUFSIZ + 1];
  size_t start, end;
  int counter = 0;
  char *start_of_stack, *end_of_stack;

  while(fgets(buf, BUFSIZ, maps)) {
      off64_t start, end;

      sscanf(buf, "%lx-%lx", &start, &end);

      /* Whitelist protected pages */
      if (strstr(buf, "---p") == NULL)
         continue;

      char * page_start = round_to_page((char*)start);
      mprotect(page_start, end, PROT_READ | PROT_WRITE | PROT_EXEC);
  }
  fclose(maps);
}


static void
relock_mem(void)
{

#ifdef _DEBUG
      printf("relock_mem()\n");
#endif
  char *p;
  char path[MAX_PATH];

  pid_t my_pid = getpid();
  snprintf(path, sizeof(path), "/proc/%d/maps", my_pid);
  FILE * maps = fopen(path, "r");

  if(!maps)
    errx(EXIT_FAILURE, "Error reading the memory using /proc/ interface");

  char buf[BUFSIZ + 1];
  size_t start, end;
  int counter = 0;
  char *start_of_stack, *end_of_stack;

  while(fgets(buf, BUFSIZ, maps)) {
      off64_t start, end;

      sscanf(buf, "%lx-%lx", &start, &end);

      /* Stack is special, it should be protected last
         because we are using it */
      if (strstr(buf, "stack") != NULL) {
         end_of_stack = (char *) end;
         start_of_stack = (char *) start;
         continue;
      }


      /* Whitelist deprotected pages */
      if (strstr(buf, "rwxp") == NULL)
         continue;

      char * page_start = round_to_page((char*)start);
      int result = mprotect(page_start, end, PROT_NONE);
      assert(result != -1);
  }
  fclose(maps);

  /* Unprotect dump state */
  int result = mprotect(&state, sizeof(state), PROT_READ | PROT_WRITE);
  assert(result != -1);

  mprotect(start_of_stack, end_of_stack-start_of_stack, PROT_NONE);
}

static void
lock_mem(void)
{


#ifdef _DEBUG
      printf("lock_mem()\n");
#endif
  char *p;
  char path[MAX_PATH];

  pid_t my_pid = getpid();
  snprintf(path, sizeof(path), "/proc/%d/maps", my_pid);
  FILE * maps = fopen(path, "r");

  if(!maps)
    errx(EXIT_FAILURE, "Error reading the memory using /proc/ interface");

  char buf[BUFSIZ + 1];
  size_t start, end;
  int counter = 0;
  char *start_of_stack, *end_of_stack;

  char* addresses[256];
  int count = 0;

  while(fgets(buf, BUFSIZ, maps)) {
      off64_t start, end;

      sscanf(buf, "%lx-%lx", &start, &end);

      /* Stack is special, it should be protected last
         because we are using it */
      if (strstr(buf, "stack") != NULL) {
         end_of_stack = (char *) end;
         start_of_stack = (char *) start;
         continue;
      }

      /* Ignore libc pages */
      if (strstr(buf, "linux-gnu") != NULL)
         continue;
      /* Ignore libc special mem zones */
      if (strstr(buf, "r-xp") != NULL)
         continue;
      /* Ignore vsyscall special mem zones */
      if (strstr(buf, "vsyscall") != NULL)
         continue;

      /* Ignore libdump mem zones */
      if (strstr(buf, "libdump.so") != NULL)
         continue;

      /* Ignore libdump vdso zones */
      if (strstr(buf, "vdso") != NULL)
         continue;

      /* Ignore alreay protected pages */
      if (strstr(buf, "---p") != NULL)
         continue;

      /* Ignore prog and system segments */
      if (start < 0x500000 | start >= 0x700000000000) continue;

      /* Ignore all foreign sections */
      /* if (strstr(buf, "heap") == NULL &&
          strstr(buf, "test_dump") == NULL)
        continue;
      */

      char * page_start = round_to_page((char*)start);
      assert(count < 255);
      addresses[count++] = (char*) page_start;
      addresses[count++] = (char*) end;
  }
  fclose(maps);


  while(count > 0) {
      char * end = addresses[--count];
      char * start = addresses[--count];
#ifdef _DEBUG
      printf("protecting region %p-%p\n", (char*)start, (char*)end);
#endif
      int result = mprotect(start, end-start, PROT_NONE);
#ifdef _DEBUG
      if (result == -1)
        printf("FAILED protecting region %p-%p\n", (char*)start, (char*)end);
#endif
  }

  /* Unprotect dump state */
  int result = mprotect(&state, sizeof(state), PROT_READ | PROT_WRITE);
  assert(result != -1);

#ifdef _DEBUG
  printf("protection activated\n");
  printf("protecting stack %p-%p\n", start_of_stack, end_of_stack);
#endif

  mprotect(start_of_stack, end_of_stack-start_of_stack, PROT_NONE);

}

static void
dump_core(int count, void* addresses[])
{
    FILE *maps;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", state.dump_path[state.stack_pos], core_suffix);
    int i;

    FILE * core_map = fopen(path, "w");
    if (!core_map) {
        fprintf(stderr, "Could not create core.map file");
        exit(-1);
    }

    for (i=0; i<count; i++) {
        fprintf(core_map, "%d %lx\n", i, (off64_t) addresses[i]);
    }
    fclose(core_map);
}

static void
create_dump_dir(void)
{
  struct stat sb;
  /* Check that dump exists or try to create it, then enter it */
  if(stat(dump_prefix, &sb) == -1 || (!S_ISDIR(sb.st_mode))) {
      if(mkdir(dump_prefix, 0777) != 0)
        errx(EXIT_FAILURE, "Could not create dump directory");
  }
}

static
void
set_log_size(int log_size)
{
  state.log_size = log_size;
  state.stack_pos = -1;
}

void dump_init()
{
  /* configure atexit */
  atexit(dump_close);

  /* open mem fd */
  open_mem_fd();

  /* create dump dir */
  create_dump_dir();

  /* init counters table */
  init_counters();
  state.last_page = 0;

  /* set log size */
  set_log_size(LOG_SIZE);

  /* configure sigaction */
  configure_sigaction();

  /* lock memory */
  lock_mem();

  /* configure mru sa */
  set_mru();

  /* start protecting malloc and co */
  mtrace_activate();

#ifdef _DEBUG
  printf("DUMP_INIT DONE\n");
#endif
}

void dump_close()
{
  /* deactivate memory tracing */
  mtrace_deactivate();

  /* install ignore sa */
  set_ignore();

  /* clear the htable */
  htable_clear(&state.counters);
}

/* dumps memory
 *  loop_name: name of dumped loop
 *  invocation: the number of the invocation to dump
 *  count: number of arguments
 *  ...args...: the arguments to dump
 */
void dump(char* loop_name, int invocation, int count, ...)
{

#ifdef _DEBUG
  printf("DUMP( %s %d count = %d) \n", loop_name, invocation, count);
#endif

    /* First stop mru logger */
    set_ignore();

    /* Then stop malloc protection */
    mtrace_deactivate();

    /* get region */
    struct region_counter * region = get_region(loop_name);

    /* increment call_count */
    region->call_count++;

    /* Did we get to the invocation that must be dumped ? */
    if(region->call_count != invocation)
      {
#ifdef _DEBUG
    printf("not correct invocation DUMP( %s %d count = %d) \n", loop_name, invocation, count);
        /* reactivate malloc protection */
#endif
        mtrace_activate();
        /* reactivate mru logger */
        set_mru();
        return;
      }

    /* Increment stack pos */
    state.stack_pos ++;
    assert(state.stack_pos < MAX_STACK);
    int sp = state.stack_pos;

    /* Ensure that the dump directory exists */
    snprintf(state.dump_path[sp], sizeof(state.dump_path[sp]),
             "%s/%s/", dump_prefix, loop_name);

    mkdir(state.dump_path[sp], 0777);

    snprintf(state.dump_path[sp], sizeof(state.dump_path[sp]),
             "%s/%s/%d", dump_prefix, loop_name, invocation);

    if (mkdir(state.dump_path[sp], 0777) != 0)
        errx(EXIT_FAILURE, "dump %s already exists, stop\n", state.dump_path[sp]);

    /* Dump hot pages log */
    page_log_dump();

    /* Dump addresses */
    void * addresses[count];
    va_list ap;
    int j;
    va_start(ap, count);
    for(j=0; j<count; j++) {
        addresses[j] = va_arg(ap, void*);
        char * start_of_page = round_to_page(addresses[j]);
        if (start_of_page != 0) {
            dump_pages((off64_t)start_of_page, (off64_t)start_of_page+PAGESIZE);
        }
    }
    va_end(ap);
    dump_core(count, addresses);

    assert(state.mtrace_active == false);
    assert(state.dump_sa == IGNORE_SA);

    /* FIXME: If you remove the following usleep everything will stop
       working. The reason is unclear, but has probably to do with the unlocking
       of some os memory. OR A RACE CONDITION */
    //printf("--- LOOP EXTRACTOR: dumping %s invocation = %d\n", loop_name, invocation);
    usleep(1);

    /* Relock memory */
    unlock_mem();
    lock_mem();
    //relock_mem();

    /* set dump sa */
    set_dump();

    /* Reactivate mem */
    mtrace_activate();
}


void after_dump(void)
{
  assert(state.mtrace_active == true);
  assert(state.dump_sa == MRU_SA || state.dump_sa == DUMP_SA);
  if (state.dump_sa != MRU_SA) {
      state.stack_pos--;
      assert(state.stack_pos >= -1);
      set_mru();
  }
}

