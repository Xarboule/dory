#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

#define PMEM_PATH "/dev/dax1.0"
//#define PMEM_PATH "/mnt/pmem0/pool"
#define FLUSH_ALIGN ((uintptr_t)64)
#define PWB_IS_CLWB

// Thank you Pedro for this part :-) (from OneFile)
//-------------------------------
#if defined(PWB_IS_CLFLUSH)

#define PWB(addr)                               \
    __asm__ volatile("clflush (%0)" ::"r"(addr) \
                     : "memory")  // Broadwell only works with this.
#define PFENCE() \
    {}  // No ordering fences needed for CLFLUSH (section 7.4.6 of Intel manual)
#define PSYNC() \
    {}  // For durability it's not obvious, but CLFLUSH seems to be enough, and
        // PMDK uses the same approach

#elif defined(PWB_IS_CLWB)
/* Use this for CPUs that support clwb, such as the SkyLake SP series (c5
 * compute intensive instances in AWS are an example of it) */
#define PWB(addr)                 \
    __asm__ volatile(             \
        ".byte 0x66; xsaveopt %0" \
        : "+m"(*(volatile char *)(addr)))  // clwb() only for Ice Lake onwards
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")

#elif defined(PWB_IS_NOP)
/* pwbs are not needed for shared memory persistency (i.e. persistency across
 * process failure) */
#define PWB(addr) \
    {}
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")

#elif defined(PWB_IS_CLFLUSHOPT)
/* Use this for CPUs that support clflushopt, which is most recent x86 */
#define PWB(addr)                \
    __asm__ volatile(            \
        ".byte 0x66; clflush %0" \
        : "+m"(*(volatile char *)(addr)))  // clflushopt (Kaby Lake)
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")
#else
#error \
    "You must define what PWB is. Choose PWB_IS_CLFLUSHOPT if you don't know what your CPU is capable of"

#endif


#define CONTENT "I AM A BEAUTIFUL PONY"
#define CONTENT_SIZE 22



//-----------------------------------------------
// From PMDK (flush.h)
void flush_with_clwb(volatile char *content, size_t count) {
    uintptr_t uptr;

    for (uptr = (uintptr_t)content & ~(FLUSH_ALIGN - 1);
         uptr < (uintptr_t)content + count; uptr += FLUSH_ALIGN) {
        PWB((char *)uptr);
    }
}

//-----------------------------------------------


typedef struct nvstruct_t {
  char content[100];
} nvstruct;



int main(int argc, char **argv){

  int mypmem_fd;
  nvstruct* nvarea = NULL;
  int check;

  //-----------------------------------------------
  
  printf("\nOpen %s file...", PMEM_PATH);
  
  mypmem_fd = open(PMEM_PATH, O_RDWR, 0);
  if (mypmem_fd == -1) {
    perror("mypmem_fd opening failed");
    return 1;
  }
  printf("    [Done]\n");

  //-----------------------------------------------
  
  printf("Mmap...");
  
  nvarea = mmap(NULL, sizeof(nvstruct),
               PROT_READ | PROT_WRITE, (MAP_SHARED_VALIDATE | MAP_SYNC), mypmem_fd, 0);
  if (nvarea == MAP_FAILED) {
        perror("Nvarea mmap failed. Check if DAX namespace align is set to 4096. (cmd: ndctl list)");
        return 1;
  }
  printf("    [Done]\n");

  //-----------------------------------------------

  printf("\n\nReading from pmem...");
  char content[100];
  strncpy(content, nvarea->content, 100);

  printf("    [Done]\n");
  printf("NVMM Content : %s\n", content);
  
  //-----------------------------------------------
  
  printf("\n\nWriting to pmem...");
  
  strncpy(nvarea->content, CONTENT, CONTENT_SIZE);
  printf("    [Done]\n");

  //-----------------------------------------------
  
  printf("\nPersistent Write Back...");
  
  flush_with_clwb(nvarea->content, CONTENT_SIZE);
  printf("    [Done]\n");

  //-----------------------------------------------
  
  printf("Munmap...");
    
  check = munmap((void *)nvarea, sizeof(nvstruct));
  if(check == -1){
    perror("Nvarea munmap failed");
    return 1;
  }
  printf("    [Done]\n\n");

  //-----------------------------------------------
  
  return 0;
}
