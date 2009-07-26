// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy/single_threaded_proxy_resolver.h"

#include "base/thread.h"
#include "net/base/net_errors.h"
#include "net/proxy/proxy_info.h"

namespace net {

// Runs on the worker thread to call ProxyResolver::SetPacScriptByUrl or
// ProxyResolver::SetPacScriptByData.
// TODO(eroman): the lifetime of this task is ill-defined; |resolver_| must
// be valid when the task eventually is run. Make this task cancellable.
class SetPacScriptTask : public Task {
 public:
  SetPacScriptTask(ProxyResolver* resolver,
                   const GURL& pac_url,
                   const std::string& bytes)
      : resolver_(resolver), pac_url_(pac_url), bytes_(bytes) {}

  virtual void Run() {
    if (resolver_->expects_pac_bytes())
      resolver_->SetPacScriptByData(bytes_);
    else
      resolver_->SetPacScriptByUrl(pac_url_);
  }

 private:
  ProxyResolver* resolver_;
  GURL pac_url_;
  std::string bytes_;
};

// SingleThreadedProxyResolver::Job -------------------------------------------

class SingleThreadedProxyResolver::Job
    : public base::RefCountedThreadSafe<SingleThreadedProxyResolver::Job> {
 public:
  // |coordinator| -- the SingleThreadedProxyResolver that owns this job.
  // |url|         -- the URL of the query.
  // |results|     -- the structure to fill with proxy resolve results.
  Job(SingleThreadedProxyResolver* coordinator,
      const GURL& url,
      ProxyInfo* results,
      CompletionCallback* callback)
    : coordinator_(coordinator),
      callback_(callback),
      results_(results),
      url_(url),
      is_started_(false),
      origin_loop_(MessageLoop::current()) {
    DCHECK(callback);
  }

  // Start the resolve proxy request on the worker thread.
  void Start() {
    is_started_ = true;
    AddRef();  // balanced in QueryComplete

    coordinator_->thread()->message_loop()->PostTask(
        FROM_HERE, NewRunnableMethod(this, &Job::DoQuery,
        coordinator_->resolver_.get()));
  }

  bool is_started() const { return is_started_; }

  void Cancel() {
    // Clear these to inform QueryComplete that it should not try to
    // access them.
    coordinator_ = NULL;
    callback_ = NULL;
    results_ = NULL;
  }

  // Returns true if Cancel() has been called.
  bool was_cancelled() const { return callback_ == NULL; }

 private:
  // Runs on the worker thread.
  void DoQuery(ProxyResolver* resolver) {
    int rv = resolver->GetProxyForURL(url_, &results_buf_, NULL, NULL);
    DCHECK_NE(rv, ERR_IO_PENDING);
    origin_loop_->PostTask(FROM_HERE,
        NewRunnableMethod(this, &Job::QueryComplete, rv));
  }

  // Runs the completion callback on the origin thread.
  void QueryComplete(int result_code) {
    // The Job may have been cancelled after it was started.
    if (!was_cancelled()) {
      if (result_code >= OK) {  // Note: unit-tests use values > 0.
        results_->Use(results_buf_);
      }
      callback_->Run(result_code);

      // We check for cancellation once again, in case the callback deleted
      // the owning ProxyService (whose destructor will in turn cancel us).
      if (!was_cancelled())
        coordinator_->RemoveFrontOfJobsQueueAndStartNext(this);
    }

    Release();  // balances the AddRef in Query.  we may get deleted after
                // we return.
  }

  // Must only be used on the "origin" thread.
  SingleThreadedProxyResolver* coordinator_;
  CompletionCallback* callback_;
  ProxyInfo* results_;
  GURL url_;
  bool is_started_;

  // Usable from within DoQuery on the worker thread.
  ProxyInfo results_buf_;
  MessageLoop* origin_loop_;
};

// SingleThreadedProxyResolver ------------------------------------------------

SingleThreadedProxyResolver::SingleThreadedProxyResolver(
    ProxyResolver* resolver)
    : ProxyResolver(resolver->expects_pac_bytes()),
      resolver_(resolver) {
}

SingleThreadedProxyResolver::~SingleThreadedProxyResolver() {
  // Cancel the inprogress job (if any), and free the rest.
  for (PendingJobsQueue::iterator it = pending_jobs_.begin();
       it != pending_jobs_.end();
       ++it) {
    (*it)->Cancel();
  }

  // Note that |thread_| is destroyed before |resolver_|. This is important
  // since |resolver_| could be running on |thread_|.
}

int SingleThreadedProxyResolver::GetProxyForURL(const GURL& url,
                                                ProxyInfo* results,
                                                CompletionCallback* callback,
                                                RequestHandle* request) {
  DCHECK(callback);

  scoped_refptr<Job> job = new Job(this, url, results, callback);
  pending_jobs_.push_back(job);
  ProcessPendingJobs();  // Jobs can never finish synchronously.

  // Completion will be notified through |callback|, unless the caller cancels
  // the request using |request|.
  if (request)
    *request = reinterpret_cast<RequestHandle>(job.get());

  return ERR_IO_PENDING;
}

// There are three states of the request we need to handle:
// (1) Not started (just sitting in the queue).
// (2) Executing Job::DoQuery in the worker thread.
// (3) Waiting for Job::QueryComplete to be run on the origin thread.
void SingleThreadedProxyResolver::CancelRequest(RequestHandle req) {
  DCHECK(req);

  Job* job = reinterpret_cast<Job*>(req);

  bool is_active_job = job->is_started() && !pending_jobs_.empty() &&
      pending_jobs_.front().get() == job;

  job->Cancel();

  if (is_active_job) {
    RemoveFrontOfJobsQueueAndStartNext(job);
    return;
  }

  // Otherwise just delete the job from the queue.
  PendingJobsQueue::iterator it = std::find(
      pending_jobs_.begin(), pending_jobs_.end(), job);
  DCHECK(it != pending_jobs_.end());
  pending_jobs_.erase(it);
}

void SingleThreadedProxyResolver::SetPacScriptByUrlInternal(
    const GURL& pac_url) {
  SetPacScriptHelper(pac_url, std::string());
}

void SingleThreadedProxyResolver::SetPacScriptByDataInternal(
    const std::string& bytes) {
  SetPacScriptHelper(GURL(), bytes);
}

void SingleThreadedProxyResolver::SetPacScriptHelper(const GURL& pac_url,
                                                     const std::string& bytes) {
  EnsureThreadStarted();
  thread()->message_loop()->PostTask(
      FROM_HERE, new SetPacScriptTask(resolver(), pac_url, bytes));
}

void SingleThreadedProxyResolver::EnsureThreadStarted() {
  if (!thread_.get()) {
    thread_.reset(new base::Thread("pac-thread"));
    thread_->Start();
  }
}

void SingleThreadedProxyResolver::ProcessPendingJobs() {
  if (pending_jobs_.empty())
    return;

  // Get the next job to process (FIFO).
  Job* job = pending_jobs_.front().get();
  if (job->is_started())
    return;

  EnsureThreadStarted();
  job->Start();
}

void SingleThreadedProxyResolver::RemoveFrontOfJobsQueueAndStartNext(
    Job* expected_job) {
  DCHECK_EQ(expected_job, pending_jobs_.front().get());
  pending_jobs_.pop_front();

  // Start next work item.
  ProcessPendingJobs();
}

}  // namespace net
