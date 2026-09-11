#ifndef TEST_LW_MUTEX_H
#define TEST_LW_MUTEX_H
#include <psp2/types.h>
typedef struct { int data[8]; } SceKernelLwMutexWork;
int sceKernelTryLockLwMutex(SceKernelLwMutexWork *,int);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *,int);
#endif
