

#ifndef ACCESSPATERN_AUTOMICS_H
#define ACCESSPATERN_AUTOMICS_H

#include <vector>

class Automics {
public:

    template<class ValueType>
    static inline bool compare_set(ValueType *valuePointer, volatile ValueType expectValue, ValueType newValue) {
        if (__atomic_compare_exchange_n(valuePointer, (ValueType *) &expectValue, newValue, false,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            return true;
        }
        return false;
    }

    template<typename T>
    static inline T
    automicIncrease(T *targetValue, long increaseNumber, int retry_num = 5) {
        if (retry_num < 0) {
            while (1) {
                volatile T expect_value = *targetValue;
                if (__atomic_compare_exchange_n(targetValue, (T *) &expect_value,
                                                expect_value + increaseNumber, false,
                                                __ATOMIC_SEQ_CST,
                                                __ATOMIC_SEQ_CST)) {
//                    if (increaseNumber == -1) {
//                        fprintf(stderr, "expect:%li, after:%li, new:%li\n", expect_value, expect_value + increaseNumber,
//                                *targetValue);
//                    }
                    return expect_value + increaseNumber;
                }
            }
        }

        for (int i = 0; i < retry_num; i++) {
            volatile T expect_value = *targetValue;
            if (__atomic_compare_exchange_n(targetValue, (T *) &expect_value, expect_value + increaseNumber,
                                            false,
                                            __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST)) {
//                if (increaseNumber == -1) {
//                    fprintf(stderr, "expect:%li, after:%li, new:%li\n", expect_value, expect_value + increaseNumber,
//                            *targetValue);
//                }
                return expect_value + increaseNumber;
            }
        }

        return -1;
    }
};

#endif //ACCESSPATERN_AUTOMICS_H
