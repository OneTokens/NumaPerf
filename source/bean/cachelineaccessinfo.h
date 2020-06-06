#ifndef ACCESSPATERN_CACHELINEACCESSINFO_H
#define ACCESSPATERN_CACHELINEACCESSINFO_H

#include "objectacessinfo.h"
#include "../xdefines.h"
#include "../utils/real.h"
#include <new>

class CacheLineAccessInfo {
private:
    unsigned long cacheLineStartAddress;
    unsigned long *threadRead;
    unsigned long *threadWrite;
    ObjectAccessInfo *residentObjectsInfoPtr[CACHE_LINE_SIZE];

    CacheLineAccessInfo() {}

    CacheLineAccessInfo(unsigned long cacheLineStartAddress) {
        this->cacheLineStartAddress = cacheLineStartAddress;
        this->threadRead = NULL;
        this->threadWrite = NULL;
    }

public:

    static CacheLineAccessInfo *createNewCacheLineAccessInfo(unsigned long cacheLineStartAddress) {
        void *buff = Real::malloc(sizeof(CacheLineAccessInfo));
        CacheLineAccessInfo *cacheLineAccessInfo = new(buff) CacheLineAccessInfo(cacheLineStartAddress);
        return cacheLineAccessInfo;
    }

    void insertResidentObject(ObjectAccessInfo *residentObjectInfoPtr) {
        unsigned long objectStartAddress = (unsigned long) residentObjectInfoPtr->getStartAddress();
        int objectIndex = objectStartAddress < cacheLineStartAddress ? 0 : objectStartAddress - cacheLineStartAddress;
        fprintf(stderr, "cache start address:%lu, index is: %d\n", cacheLineStartAddress, objectIndex);
        assert(objectIndex < CACHE_LINE_SIZE);
        residentObjectsInfoPtr[objectIndex] = residentObjectInfoPtr;
    }

    ObjectAccessInfo *findObjectInCacheLine(unsigned long address) {
        assert(address >= cacheLineStartAddress);
        assert(address <= cacheLineStartAddress + CACHE_LINE_SIZE);
        unsigned long index = address - cacheLineStartAddress;
        return residentObjectsInfoPtr[index];
    }
};

#endif //ACCESSPATERN_CACHELINEACCESSINFO_H