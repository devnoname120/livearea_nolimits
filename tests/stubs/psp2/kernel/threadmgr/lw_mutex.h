#ifndef TEST_LW_MUTEX_H
#define TEST_LW_MUTEX_H
#include <psp2/types.h>
typedef struct { int data[8]; } SceKernelLwMutexWork;
int sceKernelCreateLwMutex(SceKernelLwMutexWork *,const char *,unsigned int,int,void *);
int sceKernelDeleteLwMutex(SceKernelLwMutexWork *);
int sceKernelLockLwMutex(SceKernelLwMutexWork *,int,unsigned int *);
int sceKernelTryLockLwMutex(SceKernelLwMutexWork *,int);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *,int);
#endif
