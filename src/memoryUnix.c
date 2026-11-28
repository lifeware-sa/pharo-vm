#include "pharovm/pharo.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>


#define roundDownToPage(v) ((v)&pageMask)
#define roundUpToPage(v) (((v)+pageSize-1)&pageMask)

sqInt uxMemoryExtraBytesLeft(sqInt includingSwap);

#if !defined(MAP_ANON)
# if defined(MAP_ANONYMOUS)
#   define MAP_ANON MAP_ANONYMOUS
# else
#   define MAP_ANON 0
# endif
#endif

/* Linux >= 4.17 (glibc >= 2.28); elsewhere (macOS) a requested address is only a hint */
#if !defined(MAP_FIXED_NOREPLACE)
# define MAP_FIXED_NOREPLACE 0
#endif

#define MAP_PROT	(PROT_READ | PROT_WRITE)

#if __OpenBSD__
#define MAP_FLAGS	(MAP_ANON | MAP_PRIVATE | MAP_STACK)
#else
#define MAP_FLAGS	(MAP_ANON | MAP_PRIVATE)
#endif

#define valign(x)	((x) & pageMask)

/*xxx THESE SHOULD BE COMMAND-LINE/ENVIRONMENT OPTIONS */
/* Note:
 *
 *   The code allows memory to be overallocated; i.e., the initial
 *   block is reserved via mmap() and then the unused portion
 *   munmap()ped from the top end.  This is INHERENTLY DANGEROUS since
 *   malloc() may randomly map new memory in the block we "reserved"
 *   and subsequently unmap()ped.  Enabling this causes crashes in
 *   Croquet, which makes heavy use of the FFI and thus calls malloc()
 *   all over the place.
 *
 *   For this reason, overallocateMemory is DISABLED by default.
 *
 *   The upshot of all this is that Squeak will claim (and hold on to)
 *   ALL of the available virtual memory (or at least 75% of it) when
 *   it starts up.  If you can't live with that, use the -memory
 *   option to allocate a fixed size heap.
 */

int overallocateMemory	= 0;

static sqInt   devZero	= -1;

#ifndef max
# define max(a, b)  (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
# define min(a, b)  (((a) < (b)) ? (a) : (b))
#endif

static sqInt pageSize = 0;
static usqInt pageMask = 0;
int mmapErrno = 0;


void
sqMakeMemoryExecutableFromTo(unsigned long startAddr, unsigned long endAddr)
{
//	sqInt firstPage = roundDownToPage(startAddr);
//	if (mprotect((void *)firstPage,
//				 endAddr - firstPage + 1,
//				 PROT_READ | PROT_WRITE | PROT_EXEC) < 0){
//		logError("mprotect(x,y,PROT_READ | PROT_WRITE | PROT_EXEC)");
//		logError("ERRNO: %d\n", errno);
//		exit(1);
//	}
}

void
sqMakeMemoryNotExecutableFromTo(unsigned long startAddr, unsigned long endAddr)
{
//	sqInt firstPage = roundDownToPage(startAddr);
	/* Arguably this is pointless since allocated memory always does include
	 * write permission.  Annoyingly the mprotect call fails on both linux &
	 * mac os x.  So make the whole thing a nop.
	 */
//	if (mprotect((void *)firstPage,
//				 endAddr - firstPage + 1,
//				 PROT_READ | PROT_WRITE) < 0)
//		logErrorFromErrno("mprotect(x,y,PROT_READ | PROT_WRITE)");
}


void* allocateJITMemory(usqInt desiredSize, usqInt desiredPosition){
	
	pageMask = ~(getpagesize() - 1);

	usqInt alignedSize = valign(max(desiredSize, 1));
	usqInt desiredBaseAddressAligned = valign(desiredPosition);
	void* result;

#if __APPLE__	
	int additionalFlags = MAP_JIT;
#else
	int additionalFlags = desiredPosition ? MAP_FIXED : 0;
#endif

	logDebug("Trying to allocate JIT memory in %p\n", (void* )desiredBaseAddressAligned);

	if (MAP_FAILED == (result = mmap((void*) desiredBaseAddressAligned, alignedSize, 
			PROT_READ | PROT_WRITE | PROT_EXEC, 
			MAP_FLAGS | additionalFlags, -1, 0))) {
		logErrorFromErrno("Could not allocate JIT memory");
		exit(1);
	}

	return result;
}


/* answer the address of (minHeapSize <= N <= desiredHeapSize) bytes of memory. */
usqInt
sqAllocateMemory(usqInt minHeapSize, usqInt desiredHeapSize, usqInt desiredBaseAddress) {
    char *heap    =  0;
    sqInt   heapSize    =  0;
    sqInt   heapLimit    =  0;

#if __APPLE__
	int additionalFlags = 0;
#else
	int additionalFlags = desiredBaseAddress ? MAP_FIXED : 0;
#endif

	pageSize = getpagesize();
	pageMask = ~(pageSize - 1);

	logDebug("Requested Size %"PRIdSQINT, desiredHeapSize);

	heapLimit = valign(max(desiredHeapSize, 1));
	if(heapLimit < desiredHeapSize){
		heapLimit += pageSize;
	}
	
	usqInt desiredBaseAddressAligned = valign(desiredBaseAddress);

	logDebug("Aligned Requested Size %"PRIdSQINT, heapLimit);

	logDebug("Trying to load the image in %p\n",
			(void* )desiredBaseAddressAligned);

	while ((!heap) && (heapLimit >= minHeapSize)) {
		if (MAP_FAILED == (heap = mmap((void*) desiredBaseAddressAligned, heapLimit, MAP_PROT, MAP_FLAGS | additionalFlags, devZero, 0))) {
			heap = 0;
			heapLimit = valign(heapLimit / 4 * 3);
		}

/*
 * If we are in linux we have the problem that maybe it gives us a memory location too high in the memory map.
 * To avoid it, we force to use the required base address
 */
#ifndef __APPLE__
		if(heap != 0 && (usqInt)heap != desiredBaseAddressAligned){

			desiredBaseAddressAligned = valign(desiredBaseAddressAligned + pageSize);

			if((usqInt)heap < desiredBaseAddress){
				logError("I cannot find a good memory address starting from: %p", (void*)desiredBaseAddress);
				return 0;
			}

			//If I overflow.
			if(desiredBaseAddress > desiredBaseAddressAligned){
				logError("I cannot find a good memory address starting from: %p", (void*)desiredBaseAddress);
				return 0;
			}

			munmap(heap, heapLimit);
			heap = 0;
		}
#endif
	}

	heapSize = heapLimit;

	if(heap){
		logDebug("Loading the image in %p\n", (void* )heap);
	}

	return (usqInt) heap;
}

/* Allocate exactly [address, address + size) or nothing; answer 1 on success.
 * MAP_FIXED_NOREPLACE fails instead of replacing an existing mapping. Where it is not
 * available (0, see above) or not known to the kernel, the address is only a hint: free a
 * block that lands elsewhere.
 */
int
sqAllocateMemoryExactlyAt(void *address, usqInt size)
{
	void *alloc = mmap(address, size, MAP_PROT, MAP_FLAGS | MAP_FIXED_NOREPLACE, devZero, 0);

	if (alloc == address)
		return 1;
	if (alloc != MAP_FAILED)
		munmap(alloc, size);
	return 0;
}

/* Deallocate a region of memory previously allocated by
 * sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto.  Cannot fail.
 */
void
sqDeallocateMemorySegmentAtOfSize(void *addr, sqInt sz)
{
	if (munmap(addr, sz) != 0)
		logErrorFromErrno("sqDeallocateMemorySegment... munmap");
}


/* Copy-on-write mapping of the perm space data file of a composed image.
 * The pages are shared with every process mapping the same file until a process writes one.
 * (Windows maps a read-only view made writable on fault, because it charges commit for every page
 * of a copy-on-write view; Unix does not.) If the file cannot be mapped, the VM reads it as before.
 */
int
sqShouldMapPermSpace(void)
{
	return 1;
}

/* Map the perm space file at exactly address. Answer 1 and store the end of the perm space region
 * in *endPointer, or answer 0 if the file is not mapped (the VM then reads it).
 * The view ends on a page boundary, so that the perm space can grow in place by allocating exactly
 * at its end: the end of the last page, past the end of the file, reads as zeros. */
int
sqMapPermSpaceFile(char *fileName, void *address, size_t size, usqInt *endPointer)
{
	int fd;
	void *view;
	size_t pageSize = (size_t) sysconf(_SC_PAGESIZE);
	size = (size + pageSize - 1) & ~(pageSize - 1);

	fd = open(fileName, O_RDONLY);
	if (fd < 0) {
		logErrorFromErrno("Could not open the perm space file to map it");
		return 0;
	}
	/* No MAP_FIXED: never replace an existing mapping, only accept the exact address. */
	view = mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	close(fd);
	if (view == MAP_FAILED) {
		logErrorFromErrno("Could not map the perm space file");
		return 0;
	}
	if (view != address) {
		munmap(view, size);
		logError("Could not map the perm space file at %p (got %p)", address, view);
		return 0;
	}
	logInfo("Perm space mapped copy-on-write at %p (%lld bytes) from %s", address, (long long) size, fileName);
	*endPointer = (usqInt) address + size;
	return 1;
}

/* With a MAP_PRIVATE mapping the kernel copies a page it writes to: nothing to do. */
EXPORT(void)
sqEnsurePermWritable(void *start, size_t size)
{
}
