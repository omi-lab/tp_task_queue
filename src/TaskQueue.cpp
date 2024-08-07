#include "tp_task_queue/TaskQueue.h"

#include "tp_utils/MutexUtils.h"
#include "tp_utils/TimeUtils.h"

#include "lib_platform/SetThreadName.h"

#include <atomic>
#include <thread>

namespace tp_task_queue
{

namespace
{
//##################################################################################################
struct TaskDetails_lt
{
  TP_REF_COUNT_OBJECTS("TaskDetails_lt");
  TP_NONCOPYABLE(TaskDetails_lt);

  Task* task{nullptr};
  int64_t nextRun{0};
  bool active{false};

  TaskDetails_lt()=default;

  ~TaskDetails_lt()
  {
    delete task;
  }
};

}

//##################################################################################################
struct TaskQueue::Private
{
  TP_REF_COUNT_OBJECTS("tp_task_queue::TaskQueue::Private");
  TP_NONCOPYABLE(Private);

  std::string threadName;

  TPMutex mutex{TPM};
  TPWaitCondition waitCondition;
  TPWaitCondition updateWaitingMessagesWaitCondition;
  TPWaitCondition threadFinishedWaitCondition;
  std::vector<TaskDetails_lt*> tasks;
  size_t nextTaskIndex{0};
  bool workDone{false};
  int64_t waitFor{INT64_MAX};

  TPMutex taskStatusMutex{TPM};
  std::vector<TaskStatus> taskStatuses;

  TPMutex statusChangedCallbacksMutex{TPM};
  std::vector<const std::function<void()>*> statusChangedCallbacks;

  size_t numberOfTaskThreads;
  size_t numberOfActiveTaskThreads{0};
  std::unique_ptr<std::thread> adminThread;
  std::atomic_bool finish{false};

  //################################################################################################
  Private(std::string threadName_, size_t nThreads):
    threadName(std::move(threadName_)),
    numberOfTaskThreads(nThreads)
  {

  }

  //################################################################################################
  void taskStatusChanged()
  {
    TP_MUTEX_LOCKER(statusChangedCallbacksMutex);
    for(auto c : statusChangedCallbacks)
      (*c)();
  }

  //################################################################################################
  void updateWaitingMessages()
  {
    bool changed = false;

    {
      TP_MUTEX_LOCKER(mutex);
      for(TaskDetails_lt* taskDetails : tasks)
      {
        if(!taskDetails->active && (taskDetails->nextRun>0 || taskDetails->task->paused()))
        {
          int64_t timeToRun = taskDetails->nextRun - tp_utils::currentTimeMS();
          if(timeToRun<0)
            taskDetails->nextRun=0;
          timeToRun = tpMax(int64_t(0), timeToRun/1000);
          taskStatusMutex.lock(TPM);
          for(TaskStatus& ts : taskStatuses)
          {
            if(ts.taskID == taskDetails->task->taskID())
            {
              if(ts.paused)
                ts.message = "Paused.";
              else if(timeToRun==0)
                ts.message = "Waiting for thread.";
              else
                ts.message = taskDetails->task->timeoutMessage() + std::to_string(timeToRun);
              changed = true;
              break;
            }
          }
          taskStatusMutex.unlock(TPM);
        }
      }
    }

    if(changed)
      taskStatusChanged();
  }

  //################################################################################################
  void addThreads()
  {
    while(numberOfActiveTaskThreads<numberOfTaskThreads)
    {
      numberOfActiveTaskThreads++;
      std::thread thread([&]()
      {
        lib_platform::setThreadName(threadName);
        TPMutexLocker lock(mutex);
        while(!finish)
        {
          if(numberOfActiveTaskThreads>numberOfTaskThreads)
            break;

          if(nextTaskIndex<tasks.size())
          {
            TaskDetails_lt* taskDetails = tasks.at(nextTaskIndex);
            nextTaskIndex++;

            if(taskDetails->active)
              continue;

            if(taskDetails->task->paused())
              continue;

            int64_t timeToRun = taskDetails->nextRun - tp_utils::currentTimeMS();

            if(timeToRun<waitFor)
              waitFor = timeToRun;

            if(timeToRun>0)
              continue;

            taskDetails->active = true;

            lock.unlock(TPM);
            workDone = true;
            auto runAgain = taskDetails->task->performTask();
            lock.lock(TPM);

            if(taskDetails->task->timeoutMS()<1 || runAgain==RunAgain::No)
            {
              {
                auto i = std::find(tasks.begin(), tasks.end(), taskDetails);
                if(i != tasks.end())
                {
                  if((i-tasks.begin())<ptrdiff_t(nextTaskIndex))
                    nextTaskIndex--;
                  tasks.erase(i);
                }
              }

              {
                TP_MUTEX_LOCKER(taskStatusMutex);
                for(size_t i=0; i<taskStatuses.size(); i++)
                {
                  if(taskStatuses.at(i).taskID == taskDetails->task->taskID())
                  {
                    tpRemoveAt(taskStatuses, i);
                    break;
                  }
                }
              }

              lock.unlock(TPM);
              TaskStatus taskStatus = taskDetails->task->taskStatus();
              taskStatus.complete = true;
              taskDetails->task->updateTaskStatus(taskStatus);
              delete taskDetails;
              lock.lock(TPM);
            }
            else
            {
              if(taskDetails->task->timeoutMS()>0)
                taskDetails->nextRun = tp_utils::currentTimeMS() + taskDetails->task->timeoutMS();
              taskDetails->active = false;
            }
          }

          if(nextTaskIndex>=tasks.size())
          {
            nextTaskIndex = 0;

            auto w = waitFor;
            waitFor = INT64_MAX;

            if(!workDone)
              waitCondition.wait(TPMc lock, w);
            else
              workDone = false;
          }
        }
        numberOfActiveTaskThreads--;
        threadFinishedWaitCondition.wakeAll();
      });

      thread.detach();
    }
  }
};

//##################################################################################################
TaskQueue::TaskQueue(const std::string& threadName, size_t nThreads):
  d(new Private(threadName, nThreads))
{  
  TP_MUTEX_LOCKER(d->mutex);
  d->addThreads();

  d->adminThread = std::make_unique<std::thread>([&]()
  {
    lib_platform::setThreadName("#"+d->threadName);

    TPMutexLocker lock(d->mutex);
    while(!d->finish)
    {
      d->updateWaitingMessagesWaitCondition.wait(TPMc lock, 1000);
      {
        TP_MUTEX_UNLOCKER(lock);
        d->updateWaitingMessages();
      }
    }
  });
}

//##################################################################################################
TaskQueue::~TaskQueue()
{  
  d->finish = true;

  {
    TPMutexLocker lock(d->mutex);
    for(TaskDetails_lt* taskDetails : d->tasks)
      taskDetails->task->cancelTask();
    d->waitCondition.wakeAll();
    while(d->numberOfActiveTaskThreads>0)
      d->threadFinishedWaitCondition.wait(TPMc lock);
  }

  d->adminThread->join();
  d->adminThread.reset();

  {
    TP_MUTEX_LOCKER(d->mutex);
    for(TaskDetails_lt* taskDetails : d->tasks)
      delete taskDetails;
  }

  delete d;
}

//##################################################################################################
size_t TaskQueue::numberOfTaskThreads() const
{
  TP_MUTEX_LOCKER(d->mutex);
  return d->numberOfTaskThreads;
}

//##################################################################################################
void TaskQueue::setNumberOfTaskThreads(size_t numberOfTaskThreads)
{
  TP_MUTEX_LOCKER(d->mutex);
  d->numberOfTaskThreads = numberOfTaskThreads;
  d->addThreads();
  d->waitCondition.wakeAll();
}

//##################################################################################################
void TaskQueue::addTask(Task* task)
{
  task->setTaskQueue(this);

  TP_MUTEX_LOCKER(d->mutex);
  auto taskDetails = new TaskDetails_lt();
  taskDetails->task = task;
  taskDetails->nextRun = tp_utils::currentTimeMS() + task->timeoutMS();
  d->tasks.push_back(taskDetails);
  d->waitCondition.wakeOne();

  d->taskStatusMutex.lock(TPM);
  d->taskStatuses.push_back(taskDetails->task->taskStatus());
  d->taskStatusMutex.unlock(TPM);

  taskDetails->task->setStatusChangedCallback([&](const TaskStatus& taskStatus)
  {
    d->taskStatusMutex.lock(TPM);
    for(TaskStatus& ts : d->taskStatuses)
    {
      if(ts.taskID == taskStatus.taskID)
      {
        int64_t rev = ts.rev;
        ts = taskStatus;
        ts.rev = rev;
        break;
      }
    }
    d->taskStatusMutex.unlock(TPM);
    d->taskStatusChanged();
  });
  d->taskStatusChanged();
}

//##################################################################################################
void TaskQueue::cancelTask(int64_t taskID)
{
  TP_MUTEX_LOCKER(d->mutex);
  for(TaskDetails_lt* taskDetails : d->tasks)
  {
    if(taskDetails->task->taskID() == taskID)
    {
      taskDetails->task->cancelTask();
      d->waitCondition.wakeAll();
      break;
    }
  }
}

//##################################################################################################
void TaskQueue::pauseTask(int64_t taskID, bool paused)
{
  TP_MUTEX_LOCKER(d->mutex);
  for(TaskDetails_lt* taskDetails : d->tasks)
  {
    if(taskDetails->task->taskID() == taskID)
    {
      taskDetails->task->setPaused(paused);
      d->waitCondition.wakeAll();
      break;
    }
  }
}

//##################################################################################################
void TaskQueue::togglePauseTask(int64_t taskID)
{
  TP_MUTEX_LOCKER(d->mutex);
  for(TaskDetails_lt* taskDetails : d->tasks)
  {
    if(taskDetails->task->taskID() == taskID)
    {
      taskDetails->task->setPaused(!taskDetails->task->paused());
      d->waitCondition.wakeAll();
      break;
    }
  }
}

//##################################################################################################
void TaskQueue::viewTaskStatus(const std::function<void(const std::vector<TaskStatus>&)>& closure)
{
  TP_MUTEX_LOCKER(d->taskStatusMutex);
  closure(d->taskStatuses);
}

//##################################################################################################
void TaskQueue::addStatusChangedCallback(const std::function<void()>* statusChangedCallback)
{
  TP_MUTEX_LOCKER(d->statusChangedCallbacksMutex);
  d->statusChangedCallbacks.push_back(statusChangedCallback);
}

//##################################################################################################
void TaskQueue::removeStatusChangedCallback(const std::function<void()>* statusChangedCallback)
{
  TP_MUTEX_LOCKER(d->statusChangedCallbacksMutex);
  tpRemoveOne(d->statusChangedCallbacks, statusChangedCallback);
}

}
