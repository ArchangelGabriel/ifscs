#include "hooking.h"
#include "alloc.h"
#include <Windows.h>

#ifdef USE_PRIVATE_HEAP
void *cm_alloc(size_t size)
{
	void *ret;
	lasterror_t lasterror;

	get_lasterrors(&lasterror);
	ret = HeapAlloc(g_heap, 0, size);
	set_lasterrors(&lasterror);
	return ret;
}

void *cm_calloc(size_t count, size_t size)
{
	void *ret;
	lasterror_t lasterror;

	get_lasterrors(&lasterror);
	ret = HeapAlloc(g_heap, HEAP_ZERO_MEMORY, count * size);
	set_lasterrors(&lasterror);
	return ret;
}

void *cm_realloc(void *ptr, size_t size)
{
	void *ret;
	lasterror_t lasterror;
	get_lasterrors(&lasterror);
	ret = HeapReAlloc(g_heap, 0, ptr, size);
	set_lasterrors(&lasterror);
	return ret;
}

void cm_free(void *ptr)
{
	lasterror_t lasterror;
	get_lasterrors(&lasterror);
	HeapFree(g_heap, 0, ptr);
	set_lasterrors(&lasterror);
}
#else
void *cm_alloc(size_t size)
{
	PVOID BaseAddress = NULL;
	SIZE_T RegionSize = size + CM_ALLOC_METASIZE + 0x1000;
	struct cm_alloc_header *hdr;
	DWORD oldprot;
	LONG status;
	
	status = pNtAllocateVirtualMemory(GetCurrentProcess(), &BaseAddress, 0, &RegionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (status < 0)
		return NULL;
	hdr = (struct cm_alloc_header *)BaseAddress;
	hdr->Magic = CM_ALLOC_MAGIC;
	hdr->Used = size + CM_ALLOC_METASIZE;
	hdr->Max = RegionSize - 0x1000;

	// add a guard page to the end of every allocation
	assert(VirtualProtect((PCHAR)BaseAddress + RegionSize - 0x1000, 0x1000, PAGE_NOACCESS, &oldprot));

	return (PCHAR)BaseAddress + CM_ALLOC_METASIZE;
}

void cm_free(void *ptr)
{
	PVOID BaseAddress;
	SIZE_T RegionSize;
	LONG status;
	struct cm_alloc_header *hdr;

	hdr = GET_CM_ALLOC_HEADER(ptr);

	assert(hdr->Magic == CM_ALLOC_MAGIC);
	BaseAddress = (PVOID)hdr;
	RegionSize = 0;
	status = pNtFreeVirtualMemory(GetCurrentProcess(), &BaseAddress, &RegionSize, MEM_RELEASE);
	assert(status >= 0);
}

void *cm_realloc(void *ptr, size_t size)
{
	struct cm_alloc_header *hdr;
	char *buf;

	hdr = GET_CM_ALLOC_HEADER(ptr);

	assert(hdr->Magic == CM_ALLOC_MAGIC);

	if (hdr->Max >= (size + CM_ALLOC_METASIZE)) {
		hdr->Used = size + CM_ALLOC_METASIZE;
		return ptr;
	}
	buf = cm_alloc(size);
	if (buf == NULL)
		return buf;
	memcpy(buf, ptr, hdr->Used - CM_ALLOC_METASIZE);
	cm_free(ptr);
	return buf;
}

#endif