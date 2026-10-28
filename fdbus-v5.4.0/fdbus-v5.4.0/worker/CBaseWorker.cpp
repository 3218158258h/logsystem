/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*-----------------------------------------------------------------------------
 * INCLUDES AND NAMESPACE
 *---------------------------------------------------------------------------*/
#include <fdbus/CBaseWorker.h>
#include <fdbus/CBaseLoopTimer.h>
#include <fdbus/CBaseFdWatch.h>
#include <utils/Log.h>
#include <fdbus/CFdEventLoop.h>
#include <fdbus/CThreadEventLoop.h>
#include <stdio.h>

namespace ipc {
namespace fdbus {
/*-----------------------------------------------------------------------------
 * CLASS IMPLEMENTATIONS
 *---------------------------------------------------------------------------*/
CBaseJob::CBaseJob(uint32_t flag)
    : mFlag(flag)
    , mSyncReq(0)
    , mWorker(0)
{
}

CBaseJob::~CBaseJob()
{
}

void CBaseJob::terminate(Ptr &ref)
{
    if (!ref)
    {
        return;
    }

    if (mSyncReq)
    {
        mSyncLock.lock();
        if (mSyncReq)
        {
            // Why +1? because the job must be referred locally.
            // Warning: ref count of the job can not be changed
            // during the sync waiting!!!
            if (ref.use_count() == (mSyncReq->mInitSharedCnt + 1))
            {
                mSyncReq->mWakeupSignal.notify_one();
                mSyncReq = 0;
            }
        }
        mSyncLock.unlock();
    }
}

CBaseLoopTimer::CBaseLoopTimer(int32_t interval, bool repeat)
    : CSysLoopTimer(interval, repeat)
    , mWorker(0)
{

}

CBaseLoopTimer::~CBaseLoopTimer()
{
    attach(0);
}

class CConfigTimerJob : public CBaseJob
{
public:
    CConfigTimerJob(CSysLoopTimer *timer, CSysLoopTimer::EEnable enb,
                    CSysLoopTimer::EEnable rpt, int32_t interval, int32_t init_value)
        : CBaseJob(JOB_FORCE_RUN)
        , mTimer(timer)
        , mEnable(enb)
        , mRepeat(rpt)
        , mInterval(interval)
        , mInitValue(init_value)
    {}
protected:
    void run(Ptr &ref)
    {
        mTimer->config(mEnable, mRepeat, mInterval, mInitValue);
    }
private:
    CSysLoopTimer *mTimer;
    CSysLoopTimer::EEnable mEnable;
    CSysLoopTimer::EEnable mRepeat;
    int32_t mInterval;
    int32_t mInitValue;
};

bool CBaseLoopTimer::enable(int32_t interval)
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysLoopTimer::config(ENABLE, DONT_CARE, interval, -1);
        }
        else
        {
            mWorker->sendAsync(new CConfigTimerJob(this, CSysLoopTimer::ENABLE,
                                                   CSysLoopTimer::DONT_CARE, interval, -1));

        }
    }
    else
    {
        return false;
    }

    return true;
}

bool CBaseLoopTimer::disable()
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysLoopTimer::config(DISABLE, DONT_CARE, -1, -1);
        }
        else
        {
            mWorker->sendAsync(new CConfigTimerJob(this,
                                                   CSysLoopTimer::DISABLE,
                                                   CSysLoopTimer::DONT_CARE,
                                                   -1, -1));
        }
    }
    else
    {
        return false;
    }

    return true;
}

bool CBaseLoopTimer::enableRepeat(int32_t interval, int32_t init_value)
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysLoopTimer::config(ENABLE, ENABLE, interval, init_value);
        }
        else
        {
            mWorker->sendAsync(new CConfigTimerJob(this,
                                                   CSysLoopTimer::ENABLE,
                                                   CSysLoopTimer::ENABLE,
                                                   interval, init_value));
        }
    }
    else
    {
        return false;
    }

    return true;
}

bool CBaseLoopTimer::enableOneShot(int32_t interval)
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysLoopTimer::config(ENABLE, DISABLE, interval, -1);
        }
        else
        {
            mWorker->sendAsync(new CConfigTimerJob(this,
                                                   CSysLoopTimer::ENABLE,
                                                   CSysLoopTimer::DISABLE,
                                                   interval, -1));
        }
    }
    else
    {
        return false;
    }

    return true;
}

class CAttachTimerJob : public CBaseJob
{
public:
    CAttachTimerJob(CSysLoopTimer *timer, bool attach, bool enb)
        : CBaseJob(JOB_FORCE_RUN)
        , mTimer(timer)
        , mAttach(attach)
        , mEnable(enb)
    {
    };
protected:
    void run(Ptr &ref)
    {
        if (mAttach)
        {
            ref->worker()->getLoop()->addTimer(mTimer, mEnable);
        }
        else
        {
            ref->worker()->getLoop()->removeTimer(mTimer);
        }
    }
private:
    CSysLoopTimer *mTimer;
    bool mAttach;
    bool mEnable;
};
bool CBaseLoopTimer::attach(CBaseWorker *worker, bool enb, bool sync)
{
    if (mWorker)
    {
        if (worker)
        {
            if (worker == mWorker)
            {
                if (enb)
                {
                    enable();
                }
                else
                {
                    disable();
                }
            }
            else
            {
                LOG_E("CBaseWorker: Timer has already attached!\n");
                return false;
            }
        }
        else
        {
            if (mWorker->isSelf())
            {
                mWorker->getLoop()->removeTimer(this);
            }
            else
            {
                bool ret = sync ? mWorker->sendSync(new CAttachTimerJob(this, false, enb)) :
                                  mWorker->sendAsync(new CAttachTimerJob(this, false, enb));
                if (!ret)
                {
                    return false;
                }
            }
            mWorker = 0;
        }
    }
    else if (worker)
    {
        mWorker = worker;
        if (mWorker->isSelf())
        {
            mWorker->getLoop()->addTimer(this, enb);
        }
        else
        {
            bool ret = sync ? worker->sendSync(new CAttachTimerJob(this, true, enb)) :
                              worker->sendAsync(new CAttachTimerJob(this, true, enb));
            if (!ret)
            {
                mWorker = 0;
                return false;
            }
        }
    }
    return true;
}

CBaseFdWatch::CBaseFdWatch(int fd, int32_t flags)
    : CSysFdWatch(fd, flags)
    , mWorker(0)
{

}

CBaseFdWatch::~CBaseFdWatch()
{
    attach(0);
}

class CEnableWatchJob : public CBaseJob
{
public:
    CEnableWatchJob(CSysFdWatch *watch, bool enable)
        : CBaseJob(JOB_FORCE_RUN)
        , mWatch(watch)
        , mEnable(enable)
    {}
protected:
    void run(Ptr &ref)
    {
        mWatch->enable(mEnable);
    }
private:
    CSysFdWatch *mWatch;
    bool mEnable;
};
bool CBaseFdWatch::enable()
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysFdWatch::enable(true);
        }
        else
        {
            mWorker->sendAsync(new CEnableWatchJob(this, true));
        }
    }
    else
    {
        return false;
    }

    return true;
}

bool CBaseFdWatch::disable()
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysFdWatch::enable(false);
        }
        else
        {
            mWorker->sendAsync(new CEnableWatchJob(this, false));
        }
    }
    else
    {
        return false;
    }

    return true;
}

class CAttachWatchJob : public CBaseJob
{
public:
    CAttachWatchJob(CSysFdWatch *watch, bool attach, bool enb, CFdEventLoop *loop)
        : CBaseJob(JOB_FORCE_RUN)
        , mWatch(watch)
        , mAttach(attach)
        , mEnable(enb)
        , mLoop(loop)
    {
    };
protected:
    void run(Ptr &ref)
    {
        if (mAttach)
        {
            mLoop->addWatch(mWatch, mEnable);
        }
        else
        {
            mLoop->removeWatch(mWatch);
        }
    }
private:
    CSysFdWatch *mWatch;
    bool mAttach;
    bool mEnable;
    CFdEventLoop *mLoop;
};
bool CBaseFdWatch::attach(CBaseWorker *worker, bool enb, bool sync)
{
    if (mWorker)
    {
        if (worker)
        {
            if (worker == mWorker)
            {
                if (enb)
                {
                    enable();
                }
                else
                {
                    disable();
                }
            }
            else
            {
                LOG_E("CBaseWorker: Watch has already attached!\n");
                return false;
            }
        }
        else
        {
            if (mWorker->getLoop()->type() != CBaseEventLoop::IO_LOOP)
            {
                LOG_E("CBaseWorker: Can only be attached to IO_LOOP!"
                      " Please attach to worker created with FDB_WORKER_ENABLE_FD_LOOP!\n");
                return false;
            }
            auto loop = fdb_dynamic_cast_if_available<CFdEventLoop *>(mWorker->getLoop());
            if (!loop)
            {
                LOG_E("CBaseWorker: No event loop is specified or bad loop type!\n");
                return false;
            }
            if (mWorker->isSelf())
            {
                loop->removeWatch(this);
            }
            else
            {
                bool ret = sync ? mWorker->sendSync(new CAttachWatchJob(this, false, enb, loop)) :
                                  mWorker->sendAsync(new CAttachWatchJob(this, false, enb, loop));
                if (!ret)
                {
                    return false;
                }
            }
            mWorker = 0;
        }
    }
    else if (worker)
    {
        mWorker = worker;
        if (mWorker->getLoop()->type() != CBaseEventLoop::IO_LOOP)
        {
            LOG_E("CBaseWorker: Can only be attached to IO_LOOP!"
                  " Please attach to worker created with FDB_WORKER_ENABLE_FD_LOOP!\n");
            return false;
        }
        auto loop = fdb_dynamic_cast_if_available<CFdEventLoop *>(mWorker->getLoop());
        if (!loop)
        {
            LOG_E("CBaseWorker: No event loop is specified or bad loop type!\n");
            return false;
        }
        if (mWorker->isSelf())
        {
            loop->addWatch(this, enb);
        }
        else
        {
            bool ret = sync ? mWorker->sendSync(new CAttachWatchJob(this, true, enb, loop)) :
                              mWorker->sendAsync(new CAttachWatchJob(this, true, enb, loop));
            if (!ret)
            {
                mWorker = 0;
                return false;
            }
        }
    }
    return true;
}

class CWatchFlagsJob : public CBaseJob
{
public:
    CWatchFlagsJob(CSysFdWatch *watch, int32_t flgs)
        : CBaseJob(JOB_FORCE_RUN)
        , mWatch(watch)
        , mFlags(flgs)
    {
    }
protected:
    void run(Ptr &ref)
    {
        mWatch->flags(mFlags);
    }
private:
    CSysFdWatch *mWatch;
    int32_t mFlags;
};

bool CBaseFdWatch::flags(int32_t flgs)
{
    if (mWorker)
    {
        if (mWorker->isSelf())
        {
            CSysFdWatch::flags(flgs);
        }
        else
        {
            mWorker->sendAsync(new CWatchFlagsJob(this, flgs));
        }
    }
    else
    {
        return false;
    }

    return true;
}

CBaseWorker::CJobQueue::CJobQueue(uint32_t max_size)
        : mMaxSize(max_size)
        , mDiscardCnt(0)
        , mEventLoop(0)
{
}

bool CBaseWorker::CJobQueue::enqueue(CBaseJob::Ptr &job, bool swap)
{
    bool ret = false;
    
    if (!mMaxSize || (mJobQueue.size() < mMaxSize))
    {
        mEventLoop->lock();
        if (!mMaxSize || (mJobQueue.size() < mMaxSize))
        {
            if (swap)
            {   // thereby once out of the scope, reference count
                // of the job keep 1, but the contents in 'job' is
                // no longer available
                CBaseJob::Ptr j;
                job.swap(j);
                mJobQueue.push_back(j);
            }
            else
            {
                mJobQueue.push_back(job);
            }
            ret = true;
        }
        mEventLoop->unlock();
        if (ret)
        {
            mEventLoop->notify();
        }
    }
    
    return ret;
}

void CBaseWorker::CJobQueue::dumpJobs(tJobContainer &job_queue)
{
    job_queue = mJobQueue;
    mJobQueue.clear();
}

void CBaseWorker::CJobQueue::discardJobs()
{
    mEventLoop->lock();
    mDiscardCnt++;
    mEventLoop->unlock();
}

void CBaseWorker::CJobQueue::pickupJobs()
{
    if (mDiscardCnt > 0)
    {
        mEventLoop->lock();
        if (mDiscardCnt > 0)
        {
            mDiscardCnt--;
        }
        mEventLoop->unlock();
    }
}

bool CBaseWorker::CJobQueue::jobDiscarded()
{
    return mDiscardCnt > 0;
}

void CBaseWorker::CJobQueue::sizeLimit(uint32_t size)
{
    mMaxSize = size;
}

uint32_t CBaseWorker::CJobQueue::sizeLimit() const
{
    return mMaxSize;
}

uint32_t CBaseWorker::CJobQueue::size() const
{
    return (uint32_t)mJobQueue.size();
}

CBaseWorker::CBaseWorker(const char* thread_name,
                         uint32_t flag,
                         uint32_t normal_queue_size,
                         uint32_t urgent_queue_size)
    : CBaseThread(thread_name)
    , mExitCode(0)
    , mEventLoop(0)
    , mNormalJobQueue(normal_queue_size)
    , mUrgentJobQueue(urgent_queue_size)
{
    init(flag);
}

CBaseWorker::~CBaseWorker()
{
    if (mEventLoop)
    {
        delete mEventLoop;
    }
}

CBaseWorker *CBaseWorker::create(const char* thread_name,
                                 uint32_t flag,
                                 uint32_t normal_queue_size,
                                 uint32_t urgent_queue_size)
{
    auto worker = new CBaseWorker(thread_name,
                                  flag,
                                  normal_queue_size,
                                  urgent_queue_size);
    worker->start(flag);
    return worker;
}

bool CBaseWorker::init(uint32_t flag)
{
#ifdef __WIN32__
        static bool init_once = true;
        if (init_once)
        {
            init_once = false;
            WORD wVersionRequested;
            WSADATA wsaData;
            int err;

            /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
            wVersionRequested = MAKEWORD(2, 2);

            err = WSAStartup(wVersionRequested, &wsaData);
            if (err != 0)
            {
                /* Tell the user that we could not find a usable */
                /* Winsock DLL.                                  */
                printf("WSAStartup failed with error: %d\n", err);
            }
        }
#endif

    if (mEventLoop)
    {
        return true;
    }
    if (flag & FDB_WORKER_ENABLE_FD_LOOP)
    {
        mEventLoop = new CFdEventLoop();
    }
    else
    {
        mEventLoop = new CThreadEventLoop();
    }
    mNormalJobQueue.eventLoop(mEventLoop);
    mUrgentJobQueue.eventLoop(mEventLoop);
    if (!mEventLoop->init(this))
    {
        LOG_E("CBaseWorker: fail to initialize event loop!\n");
        return false;
    }
    return true;
}

bool CBaseWorker::start(uint32_t flag)
{
    return CBaseThread::start(flag);
}

void CBaseWorker::run()
{
    mExitCode = 0;
    try
    {
        if (!tearup())
        {
            return;
        }
    }
    catch (...)
    {
        LOG_E("CBaseWorker: exception caught in worker tearup!\n");
        return;
    }

    while (!mExitCode)
    {
        try
        {
            mEventLoop->dispatch();
        }
        catch (...)
        {
            LOG_E("CBaseWorker: exception caught in worker main loop!\n");
        }
    }

    try
    {
        teardown();
    }
    catch (...)
    {
        LOG_E("CBaseWorker: exception caught in worker teardown!\n");
    }
}

void CBaseWorker::runOneJob(tJobContainer::iterator &it, bool run_job)
{
    if (run_job)
    {
        try
        {
            (*it)->worker(this);
            (*it)->run(*it);
        }
        catch (...)
        {
            LOG_E("CBaseWorker: exception when run job!\n");
        }
    }
    (*it)->terminate(*it);
}

bool CBaseWorker::jobQueued() const
{
    return mNormalJobQueue.size() || mUrgentJobQueue.size();
}

void CBaseWorker::processUrgentJobs(tJobContainer &jobs)
{
    for (auto it = jobs.begin(); it != jobs.end(); ++it)
    {
        (*it)->urgent(true);
        bool run_job = !mUrgentJobQueue.jobDiscarded() || (*it)->forceRun();
        runOneJob(it, run_job);
    }
}

void CBaseWorker::processUrgentJobs()
{
    if (!mUrgentJobQueue.jobQueue().empty())
    {
        mEventLoop->lock();
        tJobContainer jobs;
        mUrgentJobQueue.dumpJobs(jobs);
        mEventLoop->unlock();

        processUrgentJobs(jobs);
    }
}

/*
 * at the entrace event loop is locked. it should be unlocked before exit!!!
 */
void CBaseWorker::processJobQueue()
{
    tJobContainer normal_jobs;
    tJobContainer urgent_jobs;
    mNormalJobQueue.dumpJobs(normal_jobs);
    mUrgentJobQueue.dumpJobs(urgent_jobs);
    mEventLoop->unlock();

    processUrgentJobs(urgent_jobs);
    for (auto it = normal_jobs.begin(); it != normal_jobs.end(); ++it)
    {
        processUrgentJobs();
        (*it)->urgent(false);
        bool run_job = (!mNormalJobQueue.jobDiscarded() || (*it)->forceRun()) && !mExitCode;
        runOneJob(it, run_job);
    }

    processUrgentJobs();
}

bool CBaseWorker::send(CBaseJob::Ptr &job, bool urgent, bool swap)
{
    bool ret;
    
    if (mExitCode || !mEventLoop)
    {
        return false;
    }

    if (urgent)
    {
        ret = mUrgentJobQueue.enqueue(job, swap);
    }
    else
    {
        ret = mNormalJobQueue.enqueue(job, swap);
    }

    return ret;
}

bool CBaseWorker::runBeforeStarted(CBaseJob::Ptr &job)
{
    if (!started())
    {
        std::lock_guard<std::mutex> _l(mMutex);
        if (!started())
        {
            try // catch exception to avoid missing post processing
            {
                job->worker(this);
                job->run(job);
            }
            catch (...)
            {
            }
            return true;
        }
    }
    return false;
}

bool CBaseWorker::sendAsync(CBaseJob::Ptr &job, bool urgent)
{
    return send(job, urgent);
}

bool CBaseWorker::sendAsyncEndeavor(CBaseJob::Ptr &job, bool urgent)
{
    if (isSelf())
    {
        // This is a shortcut and break the queue order. DO NOT invoke synchronously
        // in the same worker the job is intended to run!!!
        try // catch exception to avoid missing post processing
        {
            job->urgent(urgent);
            job->worker(this);
            job->run(job);
        }
        catch (...)
        {
        }
        return true;
    }
    return sendAsync(job, urgent);
}

bool CBaseWorker::sendAsyncSwap(CBaseJob::Ptr &job, bool urgent)
{
    return send(job, urgent, true);
}

bool CBaseWorker::sendAsyncSwapEndeavor(CBaseJob::Ptr &job, bool urgent)
{
    if (isSelf())
    {
        // This is a shortcut and break the queue order. DO NOT invoke synchronously
        // in the same worker the job is intended to run!!!
        try // catch exception to avoid missing post processing
        {
            job->urgent(urgent);
            job->worker(this);
            job->run(job);
        }
        catch (...)
        {
        }
        return true;
    }
    return send(job, urgent, true);
}

bool CBaseWorker::sendAsync(CBaseJob *job, bool urgent)
{
    CBaseJob::Ptr j(job);
    return sendAsyncSwap(j, urgent);
}

bool CBaseWorker::sendAsyncEndeavor(CBaseJob *job, bool urgent)
{
    CBaseJob::Ptr j(job);
    return sendAsyncSwapEndeavor(j, urgent);
}

bool CBaseWorker::send(CBaseJob::Ptr &job, int32_t milliseconds, bool urgent)
{
    if (job->mSyncReq)
    {
        return false;
    }

    // now we can assure the job is not in any worker queue
    CBaseJob::CSyncRequest sync_req(job.use_count());
    job->mSyncReq = &sync_req;
    if (!send(job, urgent))
    {
        return false;
    }

    if (job->mSyncReq)
    {
        job->mSyncLock.lock();
        if (job->mSyncReq)
        {
            if (milliseconds <= 0)
            {    // job->mSyncLock will be released
                sync_req.mWakeupSignal.wait(job->mSyncLock);
            }
            else
            {    // job->mSyncLock will be released
                std::cv_status status = sync_req.mWakeupSignal.wait_for(job->mSyncLock,
                                            std::chrono::milliseconds(milliseconds));
                if (status == std::cv_status::timeout)
                { // timeout! nothing to do.
                }
            }
            job->mSyncReq = 0;
        }
        job->mSyncLock.unlock();
    }


    return true;
}

bool CBaseWorker::sendSync(CBaseJob::Ptr &job, int32_t milliseconds, bool urgent)
{
    return send(job, milliseconds, urgent);
}

bool CBaseWorker::sendSyncEndeavor(CBaseJob::Ptr &job, int32_t milliseconds, bool urgent)
{
    if (isSelf())
    {
        // This is a shortcut and break the queue order. DO NOT invoke synchronously
        // in the same worker the job is intended to run!!!
        try // catch exception to avoid missing post processing
        {
            job->urgent(urgent);
            job->worker(this);
            job->run(job);
        }
        catch (...)
        {
        }
        return true;
    }
    return send(job, milliseconds, urgent);

}

bool CBaseWorker::sendSync(CBaseJob *job, int32_t milliseconds, bool urgent)
{
    CBaseJob::Ptr j(job);
    return sendSync(j, milliseconds, urgent);
}

bool CBaseWorker::sendSyncEndeavor(CBaseJob *job, int32_t milliseconds, bool urgent)
{
    CBaseJob::Ptr j(job);
    return sendSyncEndeavor(j, milliseconds, urgent);
}

bool CBaseWorker::sendAsync(tJobCallable job_func, bool urgent, uint32_t job_flag)
{
    return sendAsync(new CFunctionJob(job_func, job_flag), urgent);
}
bool CBaseWorker::sendAsyncEndeavor(tJobCallable job_func, bool urgent, uint32_t job_flag)
{
    if (isSelf())
    {
        // This is a shortcut and break the queue order. DO NOT invoke synchronously
        // in the same worker the job is intended to run!!!
        try // catch exception to avoid missing post processing
        {
            CBaseJob::Ptr j(new CFunctionJob(job_func, job_flag));
            j->urgent(urgent);
            j->worker(this);
            job_func(j);
        }
        catch (...)
        {
        }
        return true;
    }
    return sendAsync(job_func, urgent, job_flag);
}

bool CBaseWorker::sendSync(tJobCallable job_func,
                           int32_t milliseconds,
                           bool urgent,
                           uint32_t job_flag)
{
    return sendSync(new CFunctionJob(job_func, job_flag), milliseconds, urgent);
}
bool CBaseWorker::sendSyncEndeavor(tJobCallable job_func,
                                   int32_t milliseconds,
                                   bool urgent,
                                   uint32_t job_flag)
{
    if (isSelf())
    {
        // This is a shortcut and break the queue order. DO NOT invoke synchronously
        // in the same worker the job is intended to run!!!
        try // catch exception to avoid missing post processing
        {
            CBaseJob::Ptr j(new CFunctionJob(job_func, job_flag));
            j->urgent(urgent);
            j->worker(this);
            job_func(j);
        }
        catch (...)
        {
        }
        return true;
    }
    return sendSync(job_func, milliseconds, urgent, job_flag);
}

class CExitRequestJob : public CBaseJob
{
public:
    CExitRequestJob(int32_t exit_code)
        : CBaseJob(JOB_FORCE_RUN)
        , mExitCode(exit_code)
    {
    }
protected:
    virtual void run(Ptr &ref)
    {
        ref->worker()->mExitCode = mExitCode;
    }
private:
    int32_t mExitCode;
};

void CBaseWorker::exit(int32_t exit_code)
{
    sendAsync(new CExitRequestJob(exit_code));
}

bool CBaseWorker::flush(int32_t milliseconds, bool urgent)
{
    if (isSelf())
    {
        mEventLoop->lock();
        processJobQueue();
        return true;
    }

    return sendSync(new CBaseJob(), milliseconds, urgent);
}

void CBaseWorker::doExit(int32_t exit_code)
{
    mExitCode = exit_code;
}

void CBaseWorker::updateDiscardStatus(bool discard, bool urgent)
{
    CJobQueue &job_queue = urgent ? mUrgentJobQueue : mNormalJobQueue;
    
    if (discard)
    {
        job_queue.discardJobs();
    }
    else
    {
        job_queue.pickupJobs();
    }
}

class CUnlockJobQueueJob : public CBaseJob
{
public:
    CUnlockJobQueueJob()
        : CBaseJob(JOB_FORCE_RUN)
    {
    }
protected:
    void run(Ptr &ref)
    {
        ref->worker()->updateDiscardStatus(false, urgent());
    }
};

void CBaseWorker::discardJobs(bool urgent)
{
    auto job = new CUnlockJobQueueJob();
    
    updateDiscardStatus(true, urgent);
    if (!sendAsync(job, urgent))
    {
        updateDiscardStatus(false, urgent);
    }
}

void CBaseWorker::discardQueuedJobs(bool urgent)
{
    discardJobs(urgent);
}

void CBaseWorker::dispatchInput(int32_t timeout)
{
    mEventLoop->dispatchInput(timeout);
}
}
}
