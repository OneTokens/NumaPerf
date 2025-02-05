#ifndef NUMAPERF_ADDRTOPAGEINDEXSHADOWMAP_H
#define NUMAPERF_ADDRTOPAGEINDEXSHADOWMAP_H

#include "../mm.hh"
#include "../addresses.h"
#include "../log/Logger.h"
#include "../concurrency/automics.h"
#include "../maths.h"
#include "../../xdefines.h"
#include "../asserts.h"

/**
 * memory layout: bool-value-bool-value-bool-value
 * @tparam KeyType
 * @tparam ValueType
 */
template<class ValueType>
class AddressToPageIndexShadowMap {

    unsigned long fragmentSize;
    unsigned long fragmentMappingBitMask;
    unsigned long fragmentMappingBitNum;
    unsigned long blockSize;
    void *startAddress[MAX_FRAGMENTS];
    spinlock lock;

    const static int META_DATA_SIZE = sizeof(short);
    const static short NOT_INSERT = 0;
    const static short INSERTING = 1;
    const static short INSERTED = 2;

private:
    inline unsigned long hashKey(unsigned long key) {
        unsigned long offsetInSegment = key & fragmentMappingBitMask;
        return ADDRESSES::getPageIndex(offsetInSegment);
    }

    inline void *getDataBlock(unsigned long key) {
        unsigned int fragmentIndex = key >> fragmentMappingBitNum;
        if (fragmentIndex >= MAX_FRAGMENTS) {
            return NULL;
        }
        if (startAddress[fragmentIndex] == NULL) {
            return NULL;
        }

        unsigned long index = hashKey(key);
        unsigned long offset = index * blockSize;
//        assert(offset < fragmentSize);
        return ((char *) startAddress[fragmentIndex]) + offset;
    }

    inline void createFragment(unsigned long key) {
        lock.lock();
        unsigned int fragmentIndex = key >> fragmentMappingBitNum;
        Asserts::assertt(fragmentIndex < MAX_FRAGMENTS, 1, (char *) "add to page shadowmemory out of fragment");
        if (startAddress[fragmentIndex] != NULL) {
            lock.unlock();
            return;
        }
        startAddress[fragmentIndex] = MM::mmapAllocatePrivate(this->fragmentSize, NULL, false, -1, true);
        Logger::info("AddressToPageIndexShadowMap create Fragment index:%d\n", fragmentIndex);
        lock.unlock();
    }

public:
    void initialize(unsigned long fragmentSize, bool needAlignToCacheLine = false) {
        for (int i = 0; i < MAX_FRAGMENTS; i++) {
            startAddress[i] = NULL;
        }
        this->fragmentSize = fragmentSize;
        if (needAlignToCacheLine) {
//            Logger::debug("AlignToCacheLine, original Size:%lu, result Size:%lu \n", sizeof(ValueType) + META_DATA_SIZE,
//                          ADDRESSES::alignUpToCacheLine(sizeof(ValueType) + META_DATA_SIZE));
            blockSize = ADDRESSES::alignUpToCacheLine(sizeof(ValueType) + META_DATA_SIZE);
        } else {
//            Logger::debug("AlignToWord, original Size:%lu, result Size:%lu \n", sizeof(ValueType) + META_DATA_SIZE,
//                          ADDRESSES::alignUpToWord(sizeof(ValueType) + META_DATA_SIZE));
            blockSize = ADDRESSES::alignUpToWord(sizeof(ValueType) + META_DATA_SIZE);
        }
        unsigned long fragmentMappingSize = (fragmentSize / blockSize) * PAGE_SIZE;
        this->fragmentMappingBitNum = Maths::getCeilingPowerOf2(fragmentMappingSize);
        this->fragmentMappingBitMask = Maths::getCeilingBitMask(fragmentMappingSize);
        this->fragmentSize = (1ul << this->fragmentMappingBitNum) / PAGE_SIZE * blockSize;
        lock.init();
    }

    inline bool insertIfAbsent(const unsigned long &key, const ValueType &value) {
        void *dataBlock = this->getDataBlock(key);
        if (NULL == dataBlock) {
            this->createFragment(key);
            dataBlock = this->getDataBlock(key);
        }
        short *metaData = (short *) dataBlock;
        if (!Automics::compare_set(metaData, NOT_INSERT, INSERTING)) {
            // busy waiting, since this could be very quick
            while (*metaData != INSERTED) {
                Logger::warn("shadow map insertIfAbsent busy waiting\n");
            }
            return false;
        }
        ValueType *valuePtr = (ValueType *) (((char *) dataBlock) + META_DATA_SIZE);
        new(valuePtr)ValueType(value);
        *metaData = INSERTED;
        return true;
    }

    inline ValueType *insert(const unsigned long &key, const ValueType &value) {
        void *dataBlock = this->getDataBlock(key);
        if (NULL == dataBlock) {
            this->createFragment(key);
            dataBlock = this->getDataBlock(key);
        }
        ValueType *valuePtr = (ValueType *) (((char *) dataBlock) + META_DATA_SIZE);
        new(valuePtr)ValueType(value);
        short *metaData = (short *) dataBlock;
        *metaData = INSERTED;
        return valuePtr;
    }

    inline ValueType *find(const unsigned long &key) {
        void *dataBlock = this->getDataBlock(key);
        if (dataBlock == NULL) {
            return NULL;
        }
        if (*((short *) dataBlock) != INSERTED) {
            return NULL;
        }
        return (ValueType *) (((char *) dataBlock) + META_DATA_SIZE);
    }

    inline void remove(const unsigned long &key) {
        void *dataBlock = this->getDataBlock(key);
        if (NULL == dataBlock) {
            return;
        }
        *((short *) dataBlock) = NOT_INSERT;
    }
};

#endif //NUMAPERF_ADDRTOPAGEINDEXSHADOWMAP_H
