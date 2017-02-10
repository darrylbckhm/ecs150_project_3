#include <iostream>
#include <unistd.h>
#include <list>
#include <vector>
#include <queue>
#include <iterator>
#include <stdint.h>
#include <cstdlib>
#include <memory.h>

#include "VirtualMachine.h"
#include "Machine.h"

#define VM_THREAD_PRIORITY_IDLE 0
#define MAX_LENGTH 512

using namespace std;

extern "C" {
  
  class Block
  {
    public:
      void *start;
      TVMMemorySize size;
      bool free;
  };

  class MemoryPool {
    public:
      TVMMemoryPoolID id;
      void *mem;
      TVMMemorySize size;
      list<Block *> blocks;

  };

  class TCB {

    public: 
      TVMMemorySize memsize;
      TVMStatus status;
      TVMTick tick;
      TVMThreadID threadID;
      TVMMutexID mutexID;
      TVMThreadPriority priority;
      TVMThreadState state;
      char *stackAddr;
      volatile unsigned int sleepCount;
      // not sleeping = 0; sleeping = 1;
      unsigned int sleep;
      unsigned int addedToQueue;
      unsigned int fileCallFlag;
      volatile unsigned int fileCallData;

      SMachineContext mcntx;

      TVMThreadEntry entry;
      void *param;

      void (*TVMMainEntry)(int, char*[]);
      void (*TVMThreadEntry)(void *);

      void AlarmCall(void *param);

      TMachineSignalStateRef sigstate;

  };

  class Mutex {
    public:
      TVMMutexID mutexID;
      TCB *owner;
      unsigned int locked;
      queue<TCB *> highWaitingQueue;
      queue<TCB *> normalWaitingQueue;
      queue<TCB *> lowWaitingQueue;
  };

  //shared memory
  void *sharedmem;
  const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;
  static vector<MemoryPool *> memoryPools;
  //static volatile unsigned int system_memory_pool_size;

  // keep track of total ticks
  volatile unsigned int ticksElapsed;

  volatile unsigned int glbl_tickms;

  // vector of all threads
  static vector<TCB*> threads;
  static vector<Mutex *> mutexes;

  // queue of threads sorted by priority
  static queue<TCB*> highQueue;
  static queue<TCB*> normalQueue;
  static queue<TCB*> lowQueue;

  static TCB *curThread;
  static TVMThreadID currentThreadID;

  // declaration of custom functions
  TVMMainEntry VMLoadModule(const char *module);
  void skeleton(void *param);
  void Scheduler(bool activate);
  void idle(void *param);
  void printThreadInfo();
  void printBlocks(MemoryPool *mempool);
  
  void printBlocks(MemoryPool *mempool)
  {
    for (vector<MemoryPool *>::iterator itr = memoryPools.begin(); itr != memoryPools.end(); itr++)
    {
      if (mempool != NULL)
      {
        if ((*itr)->id == mempool->id)
        {
          list<Block *> blocks = (*itr)->blocks;
          cout << endl;
          cout << "MemPool id: " << (*itr)->id << endl;
          for (list<Block *>::iterator itr2 = blocks.begin(); itr2 != blocks.end(); itr2++)
          {
            cout << "start: " << (*itr2)->start << ", ";
            cout << "size: " << (*itr2)->size << ", ";
            cout << "free: " << (*itr2)->free << endl;
          }
          cout << endl;
        }
      }

    }
  }

  void printThreadInfo()
  {
    cout << endl;
    cout << "size of threads vector: " << threads.size() << endl;
    cout << "size of highQueue: " << highQueue.size() << endl;
    cout << "size of normalQueue: " << normalQueue.size() << endl;
    cout << "size of lowQueue: " << lowQueue.size() << endl;
    cout << "currentThread: " << curThread->threadID << endl;

    for (vector<TCB *>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {
      TCB *thread = *itr;
      cout << "threadID: " << thread->threadID << ", ";
      cout << "threadState: ";
      switch(thread->state) {
        case VM_THREAD_STATE_DEAD:  cout << "Dead";
                                    break;
        case VM_THREAD_STATE_RUNNING:  cout << "Running";
                                       break;
        case VM_THREAD_STATE_READY:  cout << "Ready";
                                     break;
        case VM_THREAD_STATE_WAITING:  cout << "Waiting";
                                       break;
        default:                    break;
      }
      cout << ", sleepStatus: " << thread->sleep;
      cout << ", addedToQueue: " << thread->addedToQueue;
      cout << endl;
    }

    for (vector<Mutex *>::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++)
    {
      Mutex *mutex = *itr;
      cout << "mutexID: " << mutex->mutexID;
      cout << ", locked: " << mutex->locked;
      TCB *thread = mutex->owner;
      if (thread)
        cout << ", owner: " << thread->threadID;
      else
        cout << ", owner: no owner";
      cout << endl;
    }

    cout << endl;
  }

  void skeleton(void *param)
  {
    MachineEnableSignals();
    TCB *thread = (TCB *)param;

    thread->entry(thread->param);

    VMThreadTerminate(curThread->threadID);

  }

  void Scheduler(bool activate)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    TCB *prevThread = curThread;
    currentThreadID = prevThread->threadID;

    for(vector<TCB*>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {

      if(((*itr)->state == VM_THREAD_STATE_READY) && ((*itr)->addedToQueue == 0))
      {

        if((*itr)->priority == VM_THREAD_PRIORITY_HIGH)
        {

          (*itr)->addedToQueue = 1;
          highQueue.push((*itr));

        }

        else if((*itr)->priority == VM_THREAD_PRIORITY_NORMAL)
        {

          (*itr)->addedToQueue = 1;
          normalQueue.push((*itr));

        }

        else if((*itr)->priority == VM_THREAD_PRIORITY_LOW)
        {

          (*itr)->addedToQueue = 1;
          lowQueue.push((*itr));

        }

      }

    }

    if(!highQueue.empty())
    {

      curThread = highQueue.front();
      highQueue.pop();

    }

    else if(!normalQueue.empty())
    {

      curThread = normalQueue.front();
      normalQueue.pop();

    }

    else if(!lowQueue.empty())
    {

      curThread = lowQueue.front();
      lowQueue.pop();

    }

    if (curThread->threadID == prevThread->threadID)
    {
      if ((threads.size() == 2) && (threads[0]->state == VM_THREAD_STATE_RUNNING))
      {
        curThread = threads[0];
        return;
      }
      curThread = threads[1];
    }

    curThread->addedToQueue = 0;

    if (activate && (curThread->priority <= prevThread->priority))
    {
      curThread = prevThread;
      MachineResumeSignals(&sigstate);
      return;
    }

    if ((curThread->priority < prevThread->priority) && (prevThread->state == VM_THREAD_STATE_RUNNING))
    {
      curThread = prevThread;
      MachineResumeSignals(&sigstate);
      return;
    }

    if (prevThread->threadID == curThread->threadID)
    {
      MachineResumeSignals(&sigstate);
      return;
    }

    if (prevThread->state == VM_THREAD_STATE_RUNNING)
      prevThread->state = VM_THREAD_STATE_READY;


    curThread->state = VM_THREAD_STATE_RUNNING; 
    MachineResumeSignals(&sigstate);
    if (MachineContextSave(&prevThread->mcntx) == 0)
    {
      MachineContextRestore(&curThread->mcntx);
    }
    //MachineContextSwitch(&prevThread->mcntx, &curThread->mcntx);

    MachineResumeSignals(&sigstate);

  }

  void idle(void *param)
  {
    while(1)
    {

      ;

    }

  }

  TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    if(mutexref == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    Mutex *mutex = new Mutex;
    mutex->locked = 0;
    mutex->mutexID = mutexes.size();

    mutexes.push_back(mutex);

    *mutexref = mutex->mutexID;

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    for (vector<Mutex *>::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++)
    {
      if ((*itr)->mutexID == mutex)
      {
        if ((*itr)->locked == 0)
        {
          (*itr)->locked = 1;
          (*itr)->owner = curThread;
        }
        else
        {

          if(timeout == VM_TIMEOUT_IMMEDIATE)
          {

            Scheduler(false);
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;

          }


          if(timeout > 0)
          {

            curThread->state = VM_THREAD_STATE_WAITING;
            curThread->sleep = 1;
            curThread->sleepCount = timeout;
            Scheduler(false);
            if((*itr)->locked == 1)
            {
              MachineResumeSignals(&sigstate);
              return VM_STATUS_FAILURE;
            }

          }
          curThread->state = VM_THREAD_STATE_WAITING;

          if(curThread->priority == VM_THREAD_PRIORITY_HIGH)
          {

            (*itr)->highWaitingQueue.push(curThread);

          }

          else if(curThread->priority == VM_THREAD_PRIORITY_NORMAL)
          {

            (*itr)->normalWaitingQueue.push(curThread);

          }

          else if(curThread->priority == VM_THREAD_PRIORITY_LOW)
          {

            (*itr)->lowWaitingQueue.push(curThread);

          }

          MachineResumeSignals(&sigstate);
          Scheduler(false);

        }
      }
      else
      {

        return VM_STATUS_ERROR_INVALID_ID;

      }
    }

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMMutexRelease(TVMMutexID mutex)
  {

    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    int found = 0;

    for (vector<Mutex *>::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++)
    {
      if ((*itr)->mutexID == mutex)
      {

        if((*itr)->owner != curThread)
        {

          MachineResumeSignals(&sigstate);
          return VM_STATUS_ERROR_INVALID_STATE;

        }

        found = 1;

        (*itr)->locked = 0;
        TCB *newOwner = NULL;
        if ((*itr)->highWaitingQueue.size() > 0)
        {
          newOwner = (*itr)->highWaitingQueue.front();
          (*itr)->highWaitingQueue.pop();
        }
        else if ((*itr)->normalWaitingQueue.size() > 0)
        {
          newOwner = (*itr)->normalWaitingQueue.front();
          (*itr)->normalWaitingQueue.pop();
        }
        else if ((*itr)->lowWaitingQueue.size() > 0)
        {
          newOwner = (*itr)->lowWaitingQueue.front();
          (*itr)->lowWaitingQueue.pop();
        }

        if (newOwner != NULL)
        {
          (*itr)->locked = 1;
          (*itr)->owner = newOwner;
          newOwner->state = VM_THREAD_STATE_READY;
          MachineResumeSignals(&sigstate);
          Scheduler(false);
        }
      }

      if(!found) 
      {

        MachineResumeSignals(&sigstate);
        return VM_STATUS_ERROR_INVALID_ID;

      }

    }

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;
  }


  TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
  {

    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    if(entry == NULL || tid == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    TVMThreadID id = threads.size();

    TCB *thread = new TCB;
    thread->memsize = memsize;
    thread->status = VM_STATUS_SUCCESS;
    thread->sleep = 0;
    thread->threadID = id;
    thread->mutexID = -1;
    thread->priority = prio;
    thread->state = VM_THREAD_STATE_DEAD;
    thread->entry =  entry;
    thread->param = param;
    VMMemoryPoolAllocate(0, thread->memsize, (void **)&(thread->stackAddr));
    MachineContextCreate(&(thread->mcntx), skeleton, thread, thread->stackAddr, thread->memsize);

    threads.push_back(thread);

    *tid = id;
    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
  {

    if(ownerref == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    int found = 0;
    
    for(vector<Mutex *>::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++)
    {

      if((*itr)->mutexID == mutex && (*itr)->locked == 1)
      {

        found = 1;
        *ownerref = ((*itr)->owner)->threadID;

      }

      if((*itr)->mutexID == mutex && (*itr)->locked == 0)
      {

        found = 1;
        *ownerref = VM_THREAD_ID_INVALID;

      }

    }

    if(!found)
      return VM_STATUS_ERROR_INVALID_ID;

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMutexDelete(TVMMutexID mutex)
  {

    int found = 0;

    for(vector<Mutex *>::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++)
    {

      if(mutex == (*itr)->mutexID)
      {

        found = 1;

        if((*itr)->locked == 1)
        {

          return VM_STATUS_ERROR_INVALID_STATE;

        }

        mutexes.erase(itr);
	break;

      }

    }

    if(!found)
      return VM_STATUS_ERROR_INVALID_ID;

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMThreadDelete(TVMThreadID thread)
  {

    int found = 0;

    for(vector<TCB*>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {

      if(thread == (*itr)->threadID)
      {

        if((*itr)->state != VM_THREAD_STATE_DEAD)
        {

          return VM_STATUS_ERROR_INVALID_STATE;

        }

        found = 1;
        threads.erase(itr);
	break;

      }

    }

    if(!found)
    {

      return VM_STATUS_ERROR_INVALID_ID;

    }

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMThreadID(TVMThreadIDRef threadref)
  {

     if(threadref == NULL)
       return VM_STATUS_ERROR_INVALID_PARAMETER;
     else
       *threadref = curThread->threadID;

     return VM_STATUS_SUCCESS;

  }

  TVMStatus VMThreadActivate(TVMThreadID thread)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    int found = 0;

    for (vector<TCB *>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {
      if ((*itr)->threadID == thread)
      {

        found = 1;

        (*itr)->state = VM_THREAD_STATE_READY;
      }
    }

    if(!found)
      return VM_STATUS_ERROR_INVALID_ID;

    Scheduler(true);
    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMThreadTerminate(TVMThreadID thread)
  {

    int found = 0;

    for(vector<TCB*>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {
      if((*itr)->threadID == thread)
      {

        found = 1; 

        if((*itr)->state == VM_THREAD_STATE_DEAD)
        {

          return VM_STATUS_ERROR_INVALID_STATE;

        }

        else
        {
	  for(vector<Mutex *>::iterator itr2 = mutexes.begin(); itr2 != mutexes.end(); itr2++)
          {

	     if ((*itr2)->owner->threadID == thread)
	     {

	       (*itr2)->locked = 0;

	     }
	
	  }

          (*itr)->state = VM_THREAD_STATE_DEAD;
          Scheduler(false);

        }

      }

    }
      if(!found) 
      {

        return VM_STATUS_ERROR_INVALID_ID;
        
      }

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
  {

    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    int found = 0;

    if(stateref == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    for (vector<TCB *>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {

      if ((*itr)->threadID == thread)
      {
        *stateref = (*itr)->state;
        found = 1;
      }

    }

    if(!found)
    {

      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;

    }

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMThreadSleep(TVMTick tick)
  {

    if(tick == VM_TIMEOUT_INFINITE)
    {

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    if(tick == VM_TIMEOUT_IMMEDIATE)
    {

      Scheduler(false);
      return VM_STATUS_SUCCESS;

    }

    for (vector<TCB *>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {
      if ((*itr)->state == VM_THREAD_STATE_RUNNING)
      {
        (*itr)->sleepCount = tick;
        (*itr)->state = VM_THREAD_STATE_WAITING;
        (*itr)->sleep = 1;
        Scheduler(false);
      }
    }

    return VM_STATUS_SUCCESS;

  }

  void AlarmCall(void *param)
  {

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    ticksElapsed++;

    for (vector<TCB *>::iterator itr = threads.begin(); itr != threads.end(); itr++)
    {
      if ((*itr)->state == VM_THREAD_STATE_WAITING)
      {
        if ((*itr)->sleep == 1)
        {
          (*itr)->sleepCount = (*itr)->sleepCount - 1;
          if ((*itr)->sleepCount == 0)
          {
            (*itr)->sleep = 0;
            (*itr)->state = VM_THREAD_STATE_READY;
          }
        }
      }
    }

    MachineResumeSignals(&sigstate);

    Scheduler(false);

  }

  TVMStatus VMTickMS(int *tickmsref)
  {

    if(tickmsref == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    *tickmsref = glbl_tickms;

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMTickCount(TVMTickRef tickref)
  {

    MachineEnableSignals();

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    if(tickref == NULL)
    {

      MachineResumeSignals(&sigstate);    
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    *tickref = ticksElapsed; 

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    int found = 0;

    if(pointer == NULL || memory == NULL)
    {

      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }
    
    for (vector<MemoryPool *>::iterator itr = memoryPools.begin(); itr != memoryPools.end(); itr++)
    {
      if ((*itr)->id == memory)
      {
        found = 1;
        list<Block *> *blocks = &(*itr)->blocks;
        bool erase = false;
        auto itr3 = blocks->begin();
        for (auto itr2 = blocks->begin(); itr2 != blocks->end(); itr2++)
        {
          if ((*itr2)->start == pointer)
          {
            Block *currentBlock = *itr2;
            Block *nextBlock = *next(itr2);
            Block *prevBlock = *prev(itr2);

            currentBlock->free = true;

            if (itr2 != prev(blocks->end()))
            {
              if (nextBlock->free == true)
              {
                //nextBlock->start = currentBlock->start;
                nextBlock->size = currentBlock->size + nextBlock->size;
                //blocks->erase(itr2);
              }
            }

            if (itr2 != blocks->begin())
            {
              if (prevBlock->free == true)
              {
                prevBlock->size = currentBlock->size + prevBlock->size;
                //blocks->erase(itr2);
                erase = true;
                itr3 = itr2;
              }
            }
            
            
          }
          else
          {

             MachineResumeSignals(&sigstate);

             return VM_STATUS_ERROR_INVALID_PARAMETER;

          }
        }

        if (erase)
          blocks->erase(itr3);

      }
    }
     
    if(!found)
    {


       MachineResumeSignals(&sigstate);

       return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    int found = 0;

    TVMMemorySize bytesleft = 0;

    VMMemoryPoolQuery(VM_MEMORY_POOL_ID_SYSTEM, &bytesleft);

    if(size > bytesleft)
    {
      
      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;

    }

    if(size == 0 || pointer == NULL)
    {

      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    TVMMemorySize trueSize;
    if ((size % 64) == 0)
    {
      trueSize = size;
    }
    else
    {
      trueSize = size + 64 - (size % 64);
    }

    //cout << endl;
    //cout << endl << "id: " << memory << endl;
    //cout << "size: " << size << endl;
    //cout << "trueSize: " << trueSize << endl;

    for (vector<MemoryPool *>::iterator itr = memoryPools.begin(); itr != memoryPools.end(); itr++)
    {
      if ((*itr)->id == memory)
      {
        found = 1;
        //cout << "found id: " << (*itr)->id << endl;
        list<Block *> *blocks = &(*itr)->blocks;
        //printBlocks(*itr);
        //for (list<Block *>::iterator itr2 = blocks.begin(); itr2 != blocks.end(); itr2++)
        for (auto itr2 = blocks->begin(); itr2 != blocks->end(); itr2++)
        {
          //cout << "size: " << (*itr2)->size << endl;
          //cout << "trueSize: " << trueSize << endl;
          //cout << "free: " << (*itr2)->free << endl;
          //cout << "freetrue?: " << ((*itr2)->free == true) << endl;
          if (((*itr2)->free == true) && ((*itr2)->size >= trueSize))
          {
            Block *free = (*itr2);
            Block *block = new Block();
            block->start = free->start;
            block->size = trueSize;
            block->free = false;
            free->start = (char *)free->start + trueSize;
            free->size = free->size - trueSize;

            blocks->insert(itr2, block);

            *pointer = block->start;

          }
        }

        auto itr2 = prev(blocks->end());
        if ((*itr2)->size == 0)
        {
          blocks->erase(itr2);
        }

        //printBlocks(*itr);
      }
    }

    if(!found)
    {

      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    } 

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    if(bytesleft == NULL)
    {

      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    int found = 0;

    TVMMemorySize sizeLeft = 0;

    for (vector<MemoryPool *>::iterator itr = memoryPools.begin(); itr != memoryPools.end(); itr++)
    {
      if ((*itr)->id == memory)
      {
        found = 1;
        //printBlocks(*itr);
        //cout << "found id: " << (*itr)->id << endl;
        list<Block *> *blocks = &(*itr)->blocks;
        //printBlocks(*itr);
        //for (list<Block *>::iterator itr2 = blocks.begin(); itr2 != blocks.end(); itr2++)
        for (auto itr2 = blocks->begin(); itr2 != blocks->end(); itr2++)
        {
          if ((*itr2)->free == true)
          {
            sizeLeft += (*itr2)->size;
          }
        }
      }
    }

    *bytesleft = sizeLeft;

    if(!found)
    {

      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    } 

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    bool found = false;
    vector<MemoryPool *>::iterator itr2 = memoryPools.begin();
    for (vector<MemoryPool *>::iterator itr = memoryPools.begin(); itr != memoryPools.end(); itr++)
    {
      if ((*itr)->id == memory)
      {
        found = true;
        itr2 = itr;
      }
    }

    if (found)
    {
      for(list<Block*>::iterator itr3 = (*itr2)->blocks.begin(); itr3 != (*itr2)->blocks.end(); itr3++)
      {

        if((*itr3)->free == 0)
        {

           MachineResumeSignals(&sigstate);

           return VM_STATUS_ERROR_INVALID_STATE;

        }

      }
      memoryPools.erase(itr2);

    }

    if(!found)
    {

       MachineResumeSignals(&sigstate);

       return VM_STATUS_ERROR_INVALID_PARAMETER;

    }
    MachineResumeSignals(&sigstate);
     

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);

    if(base == NULL || size == 0 || memory == NULL)
    {
      
      MachineResumeSignals(&sigstate);

      return VM_STATUS_ERROR_INVALID_PARAMETER;

    }

    MemoryPool *memPool = new MemoryPool;
    memPool->id = memoryPools.size();
    memPool->size = size;
    memPool->mem = base;

    Block *block = new Block();
    block->start = base;
    block->size = size;
    block->free = true;
    memPool->blocks.push_back(block);
    memoryPools.push_back(memPool);

    *memory = memPool->id;

    //cout << endl;
    //cout << "memPool id: " << memPool->id << endl;
    //cout << "memory id: " <<  *memory << endl;

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, int argc, char *argv[])
  {
    //cout << "heapsize: " << heapsize << endl;
    //cout << "sharedsize: " << sharedsize << endl;
    // create system memory pool
    MemoryPool *systemMemoryPool = new MemoryPool;
    systemMemoryPool->id = VM_MEMORY_POOL_ID_SYSTEM;
    systemMemoryPool->mem = (char *)malloc(heapsize * sizeof(char));
    systemMemoryPool->size = heapsize;

    // create free block of memory
    Block *block = new Block();
    block->start = systemMemoryPool->mem;
    block->size = heapsize;
    block->free = true;
    systemMemoryPool->blocks.push_back(block);

    memoryPools.push_back(systemMemoryPool);

    glbl_tickms = tickms;

    string module_name(argv[0]);
    TVMMainEntry main_entry = VMLoadModule(module_name.c_str());

    if(main_entry != NULL)
    {
      // create shared memory pool
      MemoryPool *sharedMemoryPool = new MemoryPool;
      sharedMemoryPool->id = memoryPools.size();
      sharedMemoryPool->mem = (char *)MachineInitialize(sharedsize);
      sharedMemoryPool->size = sharedsize;

      // create free block of memory
      Block *block = new Block();
      block->start = sharedMemoryPool->mem;
      block->size = sharedsize;
      block->free = true;
      sharedMemoryPool->blocks.push_back(block);

      memoryPools.push_back(sharedMemoryPool);

      sharedmem = memoryPools[1]->mem;
      
      MachineRequestAlarm(tickms*1000, AlarmCall, NULL);
      MachineEnableSignals();   

      TVMThreadID VMThreadID;
      VMThreadCreate(skeleton, NULL, 0x100000, VM_THREAD_PRIORITY_NORMAL, &VMThreadID);
      threads[0]->state = VM_THREAD_STATE_RUNNING;
      curThread = threads[0];

      VMThreadCreate(idle, NULL, 0x100000, VM_THREAD_PRIORITY_IDLE, &VMThreadID);
      VMThreadActivate(VMThreadID);

      main_entry(argc, argv);

    }

     else
     {

       return VM_STATUS_FAILURE;

     }

    return VM_STATUS_SUCCESS;

  }

  void fileCallback(void *calldata, int result)
  {

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    curThread->fileCallFlag = 1;

    TCB* thread = (TCB*)calldata;

    if(result > 0)
      thread->fileCallData = result;

    thread->state = VM_THREAD_STATE_READY;

    MachineResumeSignals(&sigstate);

    Scheduler(false);    

  }

  TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
  {

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    curThread->fileCallFlag = 0;

    if (newoffset != NULL)
      *newoffset = offset;

    else
      return VM_STATUS_FAILURE;

    MachineFileSeek(filedescriptor, offset, whence, fileCallback, curThread);

    curThread->state = VM_THREAD_STATE_WAITING;

    MachineResumeSignals(&sigstate);

    Scheduler(false);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMFileClose(int filedescriptor)
  {

    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    curThread->fileCallFlag = 0;

    MachineFileClose(filedescriptor, fileCallback, curThread);

    MachineResumeSignals(&sigstate);

    Scheduler(false);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
  {

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    curThread->fileCallFlag = 0;
    if (filename == NULL || filedescriptor == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineFileOpen(filename, flags, mode, fileCallback, curThread);

    curThread->state = VM_THREAD_STATE_WAITING;

    Scheduler(false);

    *filedescriptor = curThread->fileCallData; 

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS; 

  }

  TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
  { 

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);

    curThread->fileCallFlag = 0;
    if (length == NULL || data == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    int tmp = *length;
    int numWrites = 0;

    while(tmp > MAX_LENGTH)
    {

      memcpy((char*)sharedmem + (curThread->threadID * MAX_LENGTH), (char*)data + (MAX_LENGTH * numWrites), MAX_LENGTH);

      MachineFileWrite(filedescriptor, (char*)sharedmem + (curThread->threadID * MAX_LENGTH), MAX_LENGTH, fileCallback, curThread);

      curThread->state = VM_THREAD_STATE_WAITING;

      Scheduler(false);

      tmp = tmp - MAX_LENGTH;

      numWrites++;

    }

    memcpy((char*)sharedmem + (curThread->threadID * MAX_LENGTH), (char*)data + (MAX_LENGTH * numWrites), (size_t)(tmp));

    MachineFileWrite(filedescriptor, (char *)sharedmem + (curThread->threadID * MAX_LENGTH), tmp, fileCallback, curThread);

    curThread->state = VM_THREAD_STATE_WAITING;

    Scheduler(false);

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
  {

    TMachineSignalState sigstate;

    MachineSuspendSignals(&sigstate);
    curThread->fileCallFlag = 0;

    if (length == NULL || data == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;


    int *tmp = length;

    while(*tmp > MAX_LENGTH)
    {
    
      MachineFileRead(filedescriptor, (char*)sharedmem + (curThread->threadID * MAX_LENGTH), MAX_LENGTH, fileCallback, curThread);
      curThread->state = VM_THREAD_STATE_WAITING;

      Scheduler(false);

      if(curThread->fileCallData > 0)
        *length = curThread->fileCallData;

      memcpy((char*)data, ((char*)sharedmem + (curThread->threadID * MAX_LENGTH)), MAX_LENGTH);

      *tmp = *tmp - MAX_LENGTH;

      continue;

    }

    MachineFileRead(filedescriptor, (char*)sharedmem + (curThread->threadID * MAX_LENGTH), *length, fileCallback, curThread);

    curThread->state = VM_THREAD_STATE_WAITING;

    Scheduler(false);

    if(curThread->fileCallData > 0)
      *length = curThread->fileCallData;

    memcpy((char*)data, (char*)sharedmem + (curThread->threadID * MAX_LENGTH), (size_t)(*length));

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

}
