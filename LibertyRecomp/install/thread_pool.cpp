#include "thread_pool.h"
#include <os/logger.h>

namespace Install
{
    ThreadPool::ThreadPool(size_t numThreads)
    {
        if (numThreads == 0)
        {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0)
            {
                numThreads = 4;  // Fallback
            }
        }
        
        LOGF_UTILITY("[ThreadPool] Creating pool with {} threads", numThreads);
        
        for (size_t i = 0; i < numThreads; i++)
        {
            m_workers.emplace_back([this]
            {
                while (true)
                {
                    std::function<void()> task;
                    
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        
                        m_condition.wait(lock, [this]
                        {
                            return m_stop || !m_tasks.empty();
                        });
                        
                        if (m_stop && m_tasks.empty())
                        {
                            return;
                        }
                        
                        task = std::move(m_tasks.front());
                        m_tasks.pop();
                    }
                    
                    m_activeTasks++;
                    
                    try
                    {
                        task();
                    }
                    catch (const std::exception& e)
                    {
                        m_failedTasks++;
                        LOGF_ERROR("[ThreadPool] Task exception: {}", e.what());
                    }
                    catch (...)
                    {
                        m_failedTasks++;
                        LOG_ERROR("[ThreadPool] Task threw unknown exception");
                    }
                    
                    m_activeTasks--;
                    m_doneCondition.notify_all();
                }
            });
        }
    }
    
    ThreadPool::~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        
        m_condition.notify_all();
        
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        
        const size_t failed = m_failedTasks.load();
        if (failed != 0)
        {
            // A swallowed task exception never reaches its caller, so at least
            // report the count loudly when the pool goes away.
            LOGF_ERROR("[ThreadPool] Pool destroyed with {} failed task(s)", failed);
        }
        else
        {
            LOG_UTILITY("[ThreadPool] Pool destroyed");
        }
    }
    
    void ThreadPool::Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_doneCondition.wait(lock, [this]
        {
            return m_tasks.empty() && m_activeTasks == 0;
        });
    }
    
    size_t ThreadPool::GetPendingCount() const
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }
    
    // MemoryBudget implementation
    
    MemoryBudget::MemoryBudget(size_t limitBytes)
        : m_limit(limitBytes)
    {
    }
    
    void MemoryBudget::Acquire(size_t bytes)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this, bytes]
        {
            return m_used.load() + bytes <= m_limit;
        });
        m_used += bytes;
    }
    
    bool MemoryBudget::TryAcquire(size_t bytes)
    {
        size_t current = m_used.load();
        while (current + bytes <= m_limit)
        {
            if (m_used.compare_exchange_weak(current, current + bytes))
            {
                return true;
            }
        }
        return false;
    }
    
    void MemoryBudget::Release(size_t bytes)
    {
        m_used -= bytes;
        m_cv.notify_all();
    }
    
} // namespace Install
