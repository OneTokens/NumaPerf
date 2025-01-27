#ifndef NUMAPERF_BASICPAGESHADOWMAP_H
#define NUMAPERF_BASICPAGESHADOWMAP_H

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

#define META_DATA_SIZE 2 // sizeof(short)
#define NOT_INSERT ((short)0)
#define INSERTING ((short)1)
#define INSERTED ((short)2)

template<class ValueType>
class AddressToPageIndexSingleFragShadowMap {

    unsigned long fragmentSize;
    unsigned long blockSize;
    void *startAddress;

private:
    inline unsigned long hashKey(unsigned long key) {
        return ADDRESSES::getPageIndex(key);
    }

    inline void *getDataBlock(unsigned long key) {
        unsigned long index = hashKey(key);
        unsigned long offset = index * blockSize;
        // todo need check
//        Asserts::assertt(offset < fragmentSize, "add to page single shadowmemory out of range");
        return ((char *) startAddress) + offset;
    }

public:
    void initialize(unsigned long fragmentSize, bool needAlignToCacheLine = false) {
        this->fragmentSize = fragmentSize;
        this->startAddress = MM::mmapAllocatePrivate(this->fragmentSize, NULL, false, -1, true);
        Logger::info("AddressToPageSingleShadowMap create Fragment startAddress:%p\n", this->startAddress);
        if (needAlignToCacheLine) {
//            Logger::debug("AddressToPageIndexSingleFragShadowMap, original Size:%lu, result Size:%lu \n",
//                          sizeof(ValueType) + META_DATA_SIZE,
//                          ADDRESSES::alignUpToCacheLine(sizeof(ValueType) + META_DATA_SIZE));
            blockSize = ADDRESSES::alignUpToCacheLine(sizeof(ValueType) + META_DATA_SIZE);
        } else {
//            Logger::debug("AddressToPageIndexSingleFragShadowMap, original Size:%lu, result Size:%lu \n",
//                          sizeof(ValueType) + META_DATA_SIZE,
//                          ADDRESSES::alignUpToWord(sizeof(ValueType) + META_DATA_SIZE));
            blockSize = ADDRESSES::alignUpToWord(sizeof(ValueType) + META_DATA_SIZE);
        }
    }

    inline bool insertIfAbsent(const unsigned long &key, const ValueType &value) {
        void *dataBlock = this->getDataBlock(key);
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

    inline void insert(const unsigned long &key, const ValueType &value) {
        void *dataBlock = this->getDataBlock(key);
        ValueType *valuePtr = (ValueType *) (((char *) dataBlock) + META_DATA_SIZE);
        new(valuePtr)ValueType(value);
        short *metaData = (short *) dataBlock;
        *metaData = INSERTED;
    }

    inline ValueType *find(const unsigned long &key) {
        void *dataBlock = this->getDataBlock(key);
        if (*((short *) dataBlock) != INSERTED) {
            return NULL;
        }
        return (ValueType *) (((char *) dataBlock) + META_DATA_SIZE);
    }

    inline void remove(const unsigned long &key) {
        void *dataBlock = this->getDataBlock(key);
//        *((short *) dataBlock) = NOT_INSERT;
        memset(dataBlock, 0, this->blockSize);
    }
};


#endif //NUMAPERF_BASICPAGESHADOWMAP_H
