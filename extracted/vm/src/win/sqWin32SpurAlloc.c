/****************************************************************************
*   PROJECT: Squeak port for Win32 (NT / Win95)
*   FILE:    sqWin32SpurAlloc.c
*   CONTENT: Virtual Memory Management For Spur
*
*   AUTHOR:  Eliot Miranda
*   EMAIL:   eliot.miranda@gmail.com
*
*****************************************************************************/

#include <windows.h>
#include <errno.h>
#include "sq.h"
#include "pharovm/debug.h"

/* Why does this have to be *here*?? eem 6/24/2014 */
#if !defined(NDEBUG)
/* in debug mode, let the system crash so that we can see where it happened */
#define EXCEPTION_WRONG_ACCESS EXCEPTION_CONTINUE_SEARCH
#else
/* in release mode, execute the exception handler notifying the user what happened */
#define EXCEPTION_WRONG_ACCESS EXCEPTION_EXECUTE_HANDLER
#endif

void *
sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto(sqInt size, void *minAddress, sqInt *allocatedSizePointer);

LONG CALLBACK sqExceptionFilter(LPEXCEPTION_POINTERS exp)
{
  /* always wrong access - we handle memory differently now */
  return EXCEPTION_WRONG_ACCESS;
}

static sqIntptr_t  pageMask;     /* bit mask for the start of a memory page */
static sqIntptr_t  pageSize;     /* size of a memory page */
static char  *minAppAddr;	/* SYSTEM_INFO lpMinimumApplicationAddress */
static char  *maxAppAddr;	/* SYSTEM_INFO lpMaximumApplicationAddress */

# define roundDownToPage(v) ((v)&pageMask)
# define roundUpToPage(v) (((v)+pageSize-1)&pageMask)

void* allocateJITMemory(usqInt desiredSize, usqInt desiredPosition){
	
	char *address, *alloc;
	usqIntptr_t alignment;
	sqInt allocBytes;
	SYSTEM_INFO sysInfo;

	/* determine page boundaries & available address space */
	GetSystemInfo(&sysInfo);
	pageSize = sysInfo.dwPageSize;
	pageMask = ~(pageSize - 1);
	minAppAddr = sysInfo.lpMinimumApplicationAddress;
	maxAppAddr = sysInfo.lpMaximumApplicationAddress;

	alignment = max(pageSize,1024*1024);
	address = (char *)(((usqInt)desiredPosition + alignment - 1) & ~(alignment - 1));

	alloc = sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto(roundUpToPage(desiredSize), address, &allocBytes);

	if (!alloc) {
		logErrorFromErrno("Could not allocate JIT memory");
		exit(1);
	}
	return alloc;
}


/************************************************************************/
/* sqAllocateMemory: Initialize virtual memory                          */
/************************************************************************/
usqInt
sqAllocateMemory(usqInt minHeapSize, usqInt desiredHeapSize, usqInt desiredBaseAddress)
{
	char *address, *alloc;
	usqIntptr_t alignment;
	sqInt allocBytes;
	SYSTEM_INFO sysInfo;

	/* determine page boundaries & available address space */
	GetSystemInfo(&sysInfo);
	pageSize = sysInfo.dwPageSize;
	pageMask = ~(pageSize - 1);
	minAppAddr = sysInfo.lpMinimumApplicationAddress;
	maxAppAddr = sysInfo.lpMaximumApplicationAddress;

	alignment = max(pageSize,1024*1024);
	address = (char *)(((usqInt)desiredBaseAddress + alignment - 1) & ~(alignment - 1));

	alloc = sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto
				(roundUpToPage(desiredHeapSize), address, &allocBytes);

	return alloc;
}

/* Allocate exactly [address, address + size) or nothing; answer 1 on success.
 * With an explicit address VirtualAlloc never moves the block: it fails if the range is in use.
 * It rounds an address that is not a multiple of 64 KB down to one; free such a block.
 */
int
sqAllocateMemoryExactlyAt(void *address, usqInt size)
{
	void *alloc = VirtualAlloc(address, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);

	if (alloc == address)
		return 1;
	if (alloc && !VirtualFree(alloc, 0, MEM_RELEASE))
		logWarn("Unable to VirtualFree memory at %p, Error: %lu", alloc, GetLastError());
	return 0;
}

#define roundDownToPage(v) ((v)&pageMask)
#define roundUpToPage(v) (((v)+pageSize-1)&pageMask)

/* Allocate a region of memory of at least size bytes, at or above minAddress.
 *  If the attempt fails, answer null.  If the attempt succeeds, answer the
 * start of the region and assign its size through allocatedSizePointer.
 *
 * This from the VirtualFree doc is rather scary:
	dwSize [in]

		The size of the region of memory to be freed, in bytes.

		If the dwFreeType parameter is MEM_RELEASE, this parameter must be 0
		(zero). The function frees the entire region that is reserved in the
		initial allocation call to VirtualAlloc.
 *
 * So we rely on the SpurMemoryManager to free exactly the segments that were
 * allocated.
 */
#define SizeForRelease(bytes) 0

static int
address_space_used(char *address, usqInt bytes)
{
	MEMORY_BASIC_INFORMATION info;
	int addressSpaceUnused;

	if (address < minAppAddr || address > maxAppAddr)
		return 1;
	if (!VirtualQuery(address, &info, sizeof(info)))
		sqMessageBox(MB_OK | MB_ICONSTOP, TEXT("VM Error:"),
					TEXT("Unable to VirtualQuery range [%p, %p), Error: %u"),
					address, (char *)address + bytes, GetLastError());

	addressSpaceUnused = info.BaseAddress == address
						&& info.RegionSize >= bytes
						&& info.State == MEM_FREE;

	return !addressSpaceUnused;
}

void *
sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto(sqInt size, void *minAddress, sqInt *allocatedSizePointer)
{
	char *address, *alloc;
	usqInt bytes, delta;

	address = (char *)roundUpToPage((usqIntptr_t)minAddress);
	bytes = roundUpToPage(size);
	delta = max(pageSize,1024*1024);

# define printProbes 0
# define printMaps 0
	while ((usqIntptr_t)(address + bytes) > (usqIntptr_t)address) {
		if (printProbes)
			logTrace("probing [%p,%p)\n", address, address + bytes);
		if (address_space_used(address, bytes)) {
			address += delta;
			continue;
		}
		alloc = VirtualAlloc(address, bytes, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);

		/* For some reason (large page support?) we can ask for a page-aligned
		 * address such as 0xNNNNf000 but VirtualAlloc will answer 0xNNNN0000.
		 * So accept allocs above minAddress rather than allocs above address
		 */
		if (alloc >= (char *)minAddress && alloc <= address + delta) {
			if (printMaps)
				logWarn("VirtualAlloc [%p,%p) above %p)\n",
						address, address+bytes, minAddress);
			*allocatedSizePointer = bytes;
			return alloc;
		}
		if (!alloc) {
			logWarn("Unable to VirtualAlloc committed memory at desired address (%lld bytes requested at %p, above %p)", bytes, address, minAddress);
			logErrorFromGetLastError("Unable to VirtualAlloc committed memory at desired address");
			return 0;
		}
		/* VirtualAlloc answered a mapping well away from where Spur prefers.
		 * Discard the mapping and try again delta higher.
		 */
		if (alloc && !VirtualFree(alloc, SizeForRelease(bytes), MEM_RELEASE)){
			logWarn("Unable to VirtualFree committed memory at desired address (%lld bytes requested at %p, above %p), Error: %lu\n",
						bytes, address, minAddress, GetLastError());
		}

		address += delta;
	}
	logWarn("Unable to VirtualAlloc committed memory at desired address");
	return 0;
}

/* Copy-on-write mapping of the perm space data file of a composed image.
 * The pages are shared with every process mapping the same file until a process writes one.
 * The view is mapped read-only, which charges almost no commit (a FILE_MAP_COPY view would charge
 * commit for all its pages), and a write fault in it makes the page copy-on-write (PAGE_WRITECOPY,
 * which charges commit for that page only), then the write is retried. Writes by the kernel
 * (e.g. ReadFile into a perm object) do not fault: they fail with ERROR_NOACCESS, see
 * sqEnsurePermWritable. If the file cannot be mapped, the VM reads it as before.
 */
int
sqShouldMapPermSpace(void)
{
	return 1;
}

static char *permMapStart = NULL;
static char *permMapEnd = NULL;
static volatile LONG permMapWriteFaults = 0;

static LONG CALLBACK
permSpaceWriteFault(PEXCEPTION_POINTERS info)
{
	PEXCEPTION_RECORD record = info->ExceptionRecord;
	char *address;
	DWORD oldProtection;

	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
	 || record->NumberParameters < 2
	 || record->ExceptionInformation[0] != 1 /* write */)
		return EXCEPTION_CONTINUE_SEARCH;
	address = (char *) record->ExceptionInformation[1];
	if (address < permMapStart || address >= permMapEnd) {
		if (permMapStart != NULL && address >= permMapStart - 0x1000000 && address < permMapEnd + 0x1000000)
			logError("Write fault at %p next to the perm space view [%p, %p), not handled", address, permMapStart, permMapEnd);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (!VirtualProtect((void *) ((usqIntptr_t) address & ~(usqIntptr_t) 4095), 4096, PAGE_WRITECOPY, &oldProtection)) {
		logError("Write fault at %p in the perm space view: VirtualProtect failed, error %lu", address, GetLastError());
		return EXCEPTION_CONTINUE_SEARCH;
	}
	InterlockedIncrement(&permMapWriteFaults);
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* Before the kernel writes into [start, start + size): make the pages of the range that are in the
 * read-only perm view copy-on-write, by touching them from user mode (the write fault goes to
 * permSpaceWriteFault, on the C stack here). The written value is the one already there. */
EXPORT(void)
sqEnsurePermWritable(void *start, size_t size)
{
	volatile char *page;
	char *first = (char *) start, *end = (char *) start + size;

	if (permMapStart == NULL || size == 0 || end <= permMapStart || first >= permMapEnd)
		return;
	if (first < permMapStart) first = permMapStart;
	if (end > permMapEnd) end = permMapEnd;
	for (page = (volatile char *) ((usqIntptr_t) first & ~(usqIntptr_t) 4095); page < end; page += 4096)
		*page = *page;
}

/* Answer whether the perm space is the read-only view: machine code writing into it then takes a
 * write fault on the Smalltalk stack (see osCogStackPageHeadroom). */
int
sqPermSpaceIsMapped(void)
{
	return permMapStart != NULL;
}

/* Map the perm space file at exactly address. Answer 1 and store the end of the perm space region
 * in *endPointer, or answer 0 if the file is not mapped (the VM then reads it).
 * The perm space must be able to grow in place, by allocating exactly at its end, and Windows
 * allocates only at multiples of the allocation granularity (64 KB). So only the largest prefix of
 * the file that is a multiple of it is mapped; the rest of the file (less than 64 KB) is read
 * into private memory allocated right after the view, up to the next multiple. */
int
sqMapPermSpaceFile(char *fileName, void *address, size_t size, usqInt *endPointer)
{
	HANDLE file, mapping;
	void *view;
	SYSTEM_INFO info;
	size_t granularity, viewSize, tailSize, tailAllocSize;
	char *tail;
	DWORD bytesRead;
	LARGE_INTEGER offset;

	GetSystemInfo(&info);
	granularity = info.dwAllocationGranularity;
	viewSize = size & ~(granularity - 1);
	tailSize = size - viewSize;
	tailAllocSize = (tailSize + granularity - 1) & ~(granularity - 1);
	tail = (char *) address + viewSize;
	if (viewSize == 0)
		return 0;

	file = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		logErrorFromGetLastError("Could not open the perm space file to map it");
		return 0;
	}
	mapping = CreateFileMappingA(file, NULL, PAGE_WRITECOPY, 0, 0, NULL);
	if (mapping == NULL) {
		logErrorFromGetLastError("Could not create the perm space file mapping");
		CloseHandle(file);
		return 0;
	}
	/* Install the handler before the view exists, so that no write can miss it. */
	permMapStart = (char *) address;
	permMapEnd = (char *) address + viewSize;
	if (AddVectoredExceptionHandler(1, permSpaceWriteFault) == NULL) {
		logErrorFromGetLastError("Could not install the perm space write fault handler");
		CloseHandle(mapping);
		CloseHandle(file);
		permMapStart = permMapEnd = NULL;
		return 0;
	}
	view = MapViewOfFileEx(mapping, FILE_MAP_READ, 0, 0, viewSize, address);
	/* The view keeps the mapping alive. */
	CloseHandle(mapping);
	if (view != address) {
		if (view != NULL) {
			UnmapViewOfFile(view);
		}
		/* The handler stays installed but never matches an empty range. */
		permMapStart = permMapEnd = NULL;
		logErrorFromGetLastError("Could not map the perm space file at its address");
		CloseHandle(file);
		return 0;
	}
	if (tailSize > 0) {
		offset.QuadPart = (LONGLONG) viewSize;
		if (!sqAllocateMemoryExactlyAt(tail, tailAllocSize)) {
			logErrorFromGetLastError("Could not allocate the end of the perm space after its view");
			goto unmap;
		}
		if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN)
		 || !ReadFile(file, tail, (DWORD) tailSize, &bytesRead, NULL)
		 || bytesRead != tailSize) {
			logErrorFromGetLastError("Could not read the end of the perm space file");
			VirtualFree(tail, 0, MEM_RELEASE);
			goto unmap;
		}
	}
	CloseHandle(file);
	logInfo("Perm space mapped read-only, copy-on-write on first write at %p (%lld of %lld bytes) from %s",
		address, (long long) viewSize, (long long) size, fileName);
	*endPointer = (usqInt) (tail + tailAllocSize);
	return 1;

unmap:
	UnmapViewOfFile(view);
	permMapStart = permMapEnd = NULL;
	CloseHandle(file);
	return 0;
}

/* Deallocate a region of memory previously allocated by
 * sqAllocateMemorySegmentOfSizeAboveAllocatedSizeInto.  Cannot fail.
 */
void
sqDeallocateMemorySegmentAtOfSize(void *addr, sqInt sz)
{
	if (!VirtualFree(addr, SizeForRelease(sz), MEM_RELEASE))
		sqMessageBox(MB_OK | MB_ICONSTOP, TEXT("VM Warning:"),
					TEXT("Unable to VirtualFree committed memory (%") TEXT(PRIuSQINT) TEXT(" bytes requested), Error: %ul"),
					sz, GetLastError());
}

# if COGVM
void
sqMakeMemoryExecutableFromTo(usqIntptr_t startAddr, usqIntptr_t endAddr)
{
	DWORD previous;
    SIZE_T size;

    size = endAddr - startAddr;
	if (!VirtualProtect((void *)startAddr,
						size,
						PAGE_EXECUTE_READWRITE,
						&previous))
		logErrorFromErrno("VirtualProtect(x,y,PAGE_EXECUTE_READWRITE)");
}

void
sqMakeMemoryNotExecutableFromTo(usqIntptr_t startAddr, usqIntptr_t endAddr)
{
	DWORD previous;
    SIZE_T size;

    size = endAddr - startAddr;
	if (!VirtualProtect((void *)startAddr,
						size,
						PAGE_READWRITE,
						&previous))
		logErrorFromErrno("VirtualProtect(x,y,PAGE_EXECUTE_READWRITE)");
}
# endif /* COGVM */

