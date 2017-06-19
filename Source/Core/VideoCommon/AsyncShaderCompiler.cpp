// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/AsyncShaderCompiler.h"
#include <thread>
#include "Common/Assert.h"
#include "Common/Logging/Log.h"

namespace VideoCommon
{
AsyncShaderCompiler::AsyncShaderCompiler()
{
}

AsyncShaderCompiler::~AsyncShaderCompiler()
{
  // Pending work can be left at shutdown.
  // The work item classes are expected to clean up after themselves.
  _assert_(!HasWorkerThreads());
  _assert_(m_completed_work.empty());
}

void AsyncShaderCompiler::QueueWorkItem(WorkItemPtr item)
{
  // If no worker threads are available, compile synchronously.
  if (!HasWorkerThreads())
  {
    item->Compile();
    m_completed_work.push_back(std::move(item));
  }
  else
  {
    std::lock_guard<std::mutex> guard(m_pending_work_lock);
    m_pending_work.push_back(std::move(item));
    m_worker_thread_wake.notify_one();
  }
}

void AsyncShaderCompiler::RetrieveWorkItems()
{
  std::deque<WorkItemPtr> completed_work;
  {
    std::lock_guard<std::mutex> guard(m_completed_work_lock);
    m_completed_work.swap(completed_work);
  }

  while (!completed_work.empty())
  {
    completed_work.front()->Retrieve();
    completed_work.pop_front();
  }
}

bool AsyncShaderCompiler::HasPendingWork()
{
  std::lock_guard<std::mutex> guard(m_pending_work_lock);
  return !m_pending_work.empty() || m_busy_workers.load() != 0;
}

void AsyncShaderCompiler::WaitUntilCompletion()
{
  while (HasPendingWork())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void AsyncShaderCompiler::WaitUntilCompletion(
    const std::function<void(size_t, size_t)>& progress_callback)
{
  if (!HasPendingWork())
    return;

  // Wait 100ms before opening a progress dialog.
  // This way, if the operation completes quickly, we don't annoy the user.
  for (u32 i = 0; i < 100; i++)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!HasPendingWork())
      return;
  }

  // Grab the number of pending items. We use this to work out how many are left.
  size_t total_items = 0;
  {
    // Safe to hold both locks here, since nowhere else does.
    std::lock_guard<std::mutex> pending_guard(m_pending_work_lock);
    std::lock_guard<std::mutex> completed_guard(m_completed_work_lock);
    total_items = m_completed_work.size() + m_pending_work.size() + m_busy_workers.load() + 1;
  }

  // Update progress while the compiles complete.
  for (;;)
  {
    size_t remaining_items;
    {
      std::lock_guard<std::mutex> pending_guard(m_pending_work_lock);
      if (m_pending_work.empty() && !m_busy_workers.load())
        break;
      remaining_items = m_pending_work.size();
    }

    progress_callback(total_items - remaining_items, total_items);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void AsyncShaderCompiler::StartWorkerThreads(u32 num_worker_threads)
{
  for (u32 i = 0; i < num_worker_threads; i++)
  {
    void* thread_param = nullptr;
    if (!WorkerThreadInitMainThread(&thread_param))
    {
      WARN_LOG(VIDEO, "Failed to initialize shader compiler worker thread.");
      break;
    }

    m_worker_thread_start_result.store(false);

    std::thread thr(&AsyncShaderCompiler::WorkerThreadEntryPoint, this, thread_param);
    m_init_event.Wait();

    if (!m_worker_thread_start_result.load())
    {
      WARN_LOG(VIDEO, "Failed to start shader compiler worker thread.");
      thr.join();
      break;
    }

    m_worker_threads.push_back(std::move(thr));
  }
}

bool AsyncShaderCompiler::HasWorkerThreads() const
{
  return !m_worker_threads.empty();
}

void AsyncShaderCompiler::StopWorkerThreads()
{
  // Signal worker threads to stop, and wake all of them.
  {
    std::lock_guard<std::mutex> guard(m_pending_work_lock);
    m_exit_flag.Set();
    m_worker_thread_wake.notify_all();
  }

  // Wait for worker threads to exit.
  for (std::thread& thr : m_worker_threads)
    thr.join();
  m_worker_threads.clear();
}

bool AsyncShaderCompiler::WorkerThreadInitMainThread(void** param)
{
  return true;
}

bool AsyncShaderCompiler::WorkerThreadInitWorkerThread(void* param)
{
  return true;
}

void AsyncShaderCompiler::WorkerThreadExit(void* param)
{
}

void AsyncShaderCompiler::WorkerThreadEntryPoint(void* param)
{
  // Initialize worker thread with backend-specific method.
  if (!WorkerThreadInitWorkerThread(param))
  {
    WARN_LOG(VIDEO, "Failed to initialize shader compiler worker.");
    m_worker_thread_start_result.store(false);
    m_init_event.Set();
    return;
  }

  m_worker_thread_start_result.store(true);
  m_init_event.Set();

  WorkerThreadRun();

  WorkerThreadExit(param);
}

void AsyncShaderCompiler::WorkerThreadRun()
{
  std::unique_lock<std::mutex> pending_lock(m_pending_work_lock);
  while (!m_exit_flag.IsSet())
  {
    m_worker_thread_wake.wait(pending_lock);

    while (!m_pending_work.empty() && !m_exit_flag.IsSet())
    {
      m_busy_workers++;
      WorkItemPtr item(std::move(m_pending_work.front()));
      m_pending_work.pop_front();
      pending_lock.unlock();

      if (item->Compile())
      {
        std::lock_guard<std::mutex> completed_guard(m_completed_work_lock);
        m_completed_work.push_back(std::move(item));
      }

      pending_lock.lock();
      m_busy_workers--;
    }
  }
}

}  // namespace VideoCommon
