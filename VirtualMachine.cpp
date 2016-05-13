#include <iostream>
#include <unistd.h>
#include <vector>
#include <queue>
#include <stdint.h>
#include <cstdlib>
#include <memory.h>

#include "VirtualMachine.h"
#include "Machine.h"

#define VM_THREAD_PRIORITY_IDLE 0
#define MAX_LENGTH 512

extern "C" {

  using namespace std;

  class MemoryPool {

    TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM;

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
        (*itr)->stackAddr = new char[(*itr)->memsize];
        size_t stacksize = (*itr)->memsize;

        MachineContextCreate(&(*itr)->mcntx, skeleton, *itr, (*itr)->stackAddr, stacksize);
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

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
  {

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
  {

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
  {

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
  {

    

    return VM_STATUS_SUCCESS;

  }

  TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, int argc, char *argv[])
  {

    sharedmem = (void*)malloc(sharedsize * sizeof(char));

    glbl_tickms = tickms;

    string module_name(argv[0]);
    TVMMainEntry main_entry = VMLoadModule(module_name.c_str());

    if(main_entry != NULL)
    {

      sharedmem = MachineInitialize(sharedsize);
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

    if(*length > MAX_LENGTH)
    {

      int *tmp;

      memcpy((char*)sharedmem, (char*)data, MAX_LENGTH);
      MachineFileWrite(filedescriptor, (char*)sharedmem, MAX_LENGTH, fileCallback, curThread);
      curThread->state = VM_THREAD_STATE_WAITING;

      Scheduler(false);

      *tmp = *length - MAX_LENGTH;

      VMFileWrite(filedescriptor, data, tmp);

    }

    memcpy((char*)sharedmem, (char*)data, (size_t)(*length));

    MachineFileWrite(filedescriptor, (char*)sharedmem, *length, fileCallback, curThread);

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

    if(*length > MAX_LENGTH)
    {

      MachineFileRead(filedescriptor, (char*)sharedmem, MAX_LENGTH, fileCallback, curThread);
      curThread->state = VM_THREAD_STATE_WAITING;

      Scheduler(false);

      if(curThread->fileCallData > 0)
        *length = curThread->fileCallData;

      memcpy((char*)data, (((char*)sharedmem)), MAX_LENGTH);

      *length = *length - MAX_LENGTH;

      VMFileRead(filedescriptor, data, length);

    }

    MachineFileRead(filedescriptor, (char*)sharedmem, *length, fileCallback, curThread);

    curThread->state = VM_THREAD_STATE_WAITING;

    Scheduler(false);

    if(curThread->fileCallData > 0)
      *length = curThread->fileCallData;

    memcpy((char*)sharedmem, (char*)data, (size_t)(*length));

    MachineResumeSignals(&sigstate);

    return VM_STATUS_SUCCESS;

  }

}
