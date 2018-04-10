#define PONY_WANT_ATOMIC_DEFS

#include "asio.h"
#include "event.h"
#ifdef ASIO_USE_EPOLL

#include "../actor/messageq.h"
#include "../mem/pool.h"
#include "../sched/cpu.h"
#include "../sched/scheduler.h"
#include "ponyassert.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>

#ifdef USE_VALGRIND
#include <valgrind/helgrind.h>
#endif

#define MAX_SIGNAL 128

struct asio_backend_t
{
  int epfd;
  int wakeup;    /* eventfd to break epoll loop */
  struct epoll_event events[MAX_EVENTS];
  PONY_ATOMIC(asio_event_t*) sighandlers[MAX_SIGNAL];
  PONY_ATOMIC(bool) terminate;
  messageq_t q;
};

static void send_request(asio_event_t* ev, int req)
{
  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  asio_msg_t* msg = (asio_msg_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(asio_msg_t)), 0);
  msg->event = ev;
  msg->flags = req;
  ponyint_thread_messageq_push(&b->q, (pony_msg_t*)msg, (pony_msg_t*)msg
#ifdef USE_DYNAMIC_TRACE
    , SPECIAL_THREADID_EPOLL, SPECIAL_THREADID_EPOLL
#endif
    );

  eventfd_write(b->wakeup, 1);
}

static void signal_handler(int sig)
{
  if(sig >= MAX_SIGNAL)
    return;

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);
  asio_event_t* ev = atomic_load_explicit(&b->sighandlers[sig],
    memory_order_acquire);

#ifdef USE_VALGRIND
  ANNOTATE_HAPPENS_AFTER(&b->sighandlers[sig]);
#endif

  if(ev == NULL)
    return;

  eventfd_write(ev->asio_event_attr.fd, 1);
}

#if !defined(USE_SCHEDULER_SCALING_PTHREADS)
static void empty_signal_handler(int sig)
{
  (void) sig;
}
#endif

static void handle_queue(asio_backend_t* b)
{
  asio_msg_t* msg;

  while((msg = (asio_msg_t*)ponyint_thread_messageq_pop(&b->q
#ifdef USE_DYNAMIC_TRACE
    , SPECIAL_THREADID_EPOLL
#endif
    )) != NULL)
  {
    asio_event_t* ev = msg->event;

    switch(msg->flags)
    {
      case ASIO_DISPOSABLE:
        pony_asio_event_send(ev, ASIO_DISPOSABLE, 0);
        break;

      default: {}
    }
  }
}

asio_backend_t* ponyint_asio_backend_init()
{
  asio_backend_t* b = POOL_ALLOC(asio_backend_t);
  memset(b, 0, sizeof(asio_backend_t));
  ponyint_messageq_init(&b->q);

  b->epfd = epoll_create1(EPOLL_CLOEXEC);
  b->wakeup = eventfd(0, EFD_NONBLOCK);

  if(b->epfd == 0 || b->wakeup == 0)
  {
    POOL_FREE(asio_backend_t, b);
    return NULL;
  }

  struct epoll_event ep;
  ep.data.ptr = b;
  ep.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

  epoll_ctl(b->epfd, EPOLL_CTL_ADD, b->wakeup, &ep);

#if !defined(USE_SCHEDULER_SCALING_PTHREADS)
  // Make sure we ignore signals related to scheduler sleeping/waking
  // as the default for those signals is termination
  struct sigaction new_action;
  new_action.sa_handler = empty_signal_handler;
  sigemptyset (&new_action.sa_mask);

  // ask to restart interrupted syscalls to match `signal` behavior
  new_action.sa_flags = SA_RESTART;

  sigaction(PONY_SCHED_SLEEP_WAKE_SIGNAL, &new_action, NULL);
#endif

  return b;
}

void ponyint_asio_backend_final(asio_backend_t* b)
{
  atomic_store_explicit(&b->terminate, true, memory_order_relaxed);
  eventfd_write(b->wakeup, 1);
}

PONY_API void pony_asio_event_resubscribe_write(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  struct epoll_event ep;
  ep.data.ptr = ev;
  ep.events = 0;

  if(ev->flags & ASIO_ONESHOT)
    ep.events |= EPOLLONESHOT;

  if((ev->flags & ASIO_WRITE) && !ev->writeable)
    ep.events |= EPOLLOUT;
  else
    return;

  epoll_ctl(b->epfd, EPOLL_CTL_MOD, ev->asio_event_attr.fd, &ep);
}

PONY_API void pony_asio_event_resubscribe_read(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  struct epoll_event ep;
  ep.data.ptr = ev;
  ep.events = EPOLLRDHUP | EPOLLET;

  if(ev->flags & ASIO_ONESHOT)
    ep.events |= EPOLLONESHOT;

  if((ev->flags & ASIO_READ) && !ev->readable)
    ep.events |= EPOLLIN;
  else
    return;

  epoll_ctl(b->epfd, EPOLL_CTL_MOD, ev->asio_event_attr.fd, &ep);
}

DECLARE_THREAD_FN(ponyint_asio_backend_dispatch)
{
  ponyint_cpu_affinity(ponyint_asio_get_cpu());
  pony_register_thread();
  asio_backend_t* b = arg;
  pony_assert(b != NULL);

#if !defined(USE_SCHEDULER_SCALING_PTHREADS)
  // Make sure we block signals related to scheduler sleeping/waking
  // so they queue up to avoid race conditions
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, PONY_SCHED_SLEEP_WAKE_SIGNAL);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
#endif

  while(!atomic_load_explicit(&b->terminate, memory_order_relaxed))
  {
    int event_cnt = epoll_wait(b->epfd, b->events, MAX_EVENTS, -1);

    for(int i = 0; i < event_cnt; i++)
    {
      struct epoll_event* ep = &(b->events[i]);

      if(ep->data.ptr == b)
        continue;

      asio_event_t* ev = ep->data.ptr;
      uint32_t flags = 0;
      uint32_t count = 0;

      if(ev->flags & ASIO_READ)
      {
        if(ep->events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        {
          flags |= ASIO_READ;
          ev->readable = true;
        }
      }

      if(ev->flags & ASIO_WRITE)
      {
        if(ep->events & EPOLLOUT)
        {
          flags |= ASIO_WRITE;
          ev->writeable = true;
        }
      }

      if(ev->flags & ASIO_TIMER)
      {
        if(ep->events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        {
          uint64_t missed;
          ssize_t rc = read(ev->asio_event_attr.fd, &missed, sizeof(uint64_t));
          (void)rc;
          flags |= ASIO_TIMER;
        }
      }

      if(ev->flags & ASIO_SIGNAL)
      {
        if(ep->events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        {
          uint64_t missed;
          ssize_t rc = read(ev->asio_event_attr.fd, &missed, sizeof(uint64_t));
          (void)rc;
          flags |= ASIO_SIGNAL;
          count = (uint32_t)missed;
        }
      }

      if(flags != 0)
      {
        if(ev->auto_resub && !(flags & ASIO_WRITE))
          pony_asio_event_resubscribe_write(ev);
        if(ev->auto_resub && !(flags & ASIO_READ))
          pony_asio_event_resubscribe_read(ev);
        pony_asio_event_send(ev, flags, count);
      }
    }

    handle_queue(b);
  }

  close(b->epfd);
  close(b->wakeup);
  ponyint_messageq_destroy(&b->q);
  POOL_FREE(asio_backend_t, b);
  pony_unregister_thread();
  return NULL;
}

static void timer_set_nsec(int fd, uint64_t nsec)
{
  struct itimerspec ts;

  ts.it_interval.tv_sec = 0;
  ts.it_interval.tv_nsec = 0;
  ts.it_value.tv_sec = (time_t)(nsec / 1000000000);
  ts.it_value.tv_nsec = (long)(nsec - (ts.it_value.tv_sec * 1000000000));

  timerfd_settime(fd, 0, &ts, NULL);
}

PONY_API void __pony_asio_event_subscribe(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if(ev->noisy)
  {
    uint64_t old_count = ponyint_asio_noisy_add();
    // tell scheduler threads that asio has at least one noisy actor
    // if the old_count was 0
    if (old_count == 0)
      ponyint_sched_noisy_asio(SPECIAL_THREADID_EPOLL);
  }

  struct epoll_event ep;
  ep.data.ptr = ev;
  ep.events = EPOLLRDHUP | EPOLLET;

  if(ev->flags & ASIO_READ)
    ep.events |= EPOLLIN;

  if(ev->flags & ASIO_WRITE)
    ep.events |= EPOLLOUT;

  if(ev->flags & ASIO_TIMER)
  {
    ev->asio_event_attr.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    timer_set_nsec(ev->asio_event_attr.fd, ev->asio_event_attr.nsec);
    ep.events |= EPOLLIN;
  }

  if(ev->flags & ASIO_SIGNAL)
  {
    int sig = ev->asio_event_attr.signal;
    asio_event_t* prev = NULL;

#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&b->sighandlers[sig]);
#endif
    if((sig < MAX_SIGNAL) &&
      atomic_compare_exchange_strong_explicit(&b->sighandlers[sig], &prev, ev,
      memory_order_release, memory_order_relaxed))
    {
      struct sigaction new_action;
      new_action.sa_handler = signal_handler;
      sigemptyset (&new_action.sa_mask);

      // ask to restart interrupted syscalls to match `signal` behavior
      new_action.sa_flags = SA_RESTART;

      sigaction(sig, &new_action, NULL);

      ev->asio_event_attr.fd = eventfd(0, EFD_NONBLOCK);
      ep.events |= EPOLLIN;
    } else {
      return;
    }
  }

  if(ev->flags & ASIO_ONESHOT) {
    ep.events |= EPOLLONESHOT;
    ev->auto_resub = true;
  }

  epoll_ctl(b->epfd, EPOLL_CTL_ADD, ev->asio_event_attr.fd, &ep);
}

PONY_API void pony_asio_event_update_nsec(asio_event_t* ev, uint64_t nsec)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  if(ev->flags & ASIO_TIMER)
  {
    ev->asio_event_attr.nsec = nsec;
    timer_set_nsec(ev->asio_event_attr.fd, nsec);
  }
}

PONY_API void pony_asio_event_unsubscribe(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if(ev->noisy)
  {
    uint64_t old_count = ponyint_asio_noisy_remove();
    // tell scheduler threads that asio has no noisy actors
    // if the old_count was 1
    if (old_count == 1)
    {
      ponyint_sched_unnoisy_asio(SPECIAL_THREADID_EPOLL);

      // maybe wake up a scheduler thread if they've all fallen asleep
      ponyint_sched_maybe_wakeup_if_all_asleep(-1);
    }

    ev->noisy = false;
  }

  epoll_ctl(b->epfd, EPOLL_CTL_DEL, ev->asio_event_attr.fd, NULL);

  if(ev->flags & ASIO_TIMER)
  {
    if(ev->asio_event_attr.fd != -1)
    {
      close(ev->asio_event_attr.fd);
      ev->asio_event_attr.fd = -1;
    }
  }

  if(ev->flags & ASIO_SIGNAL)
  {
    int sig = ev->asio_event_attr.signal;
    asio_event_t* prev = ev;

#ifdef USE_VALGRIND
    ANNOTATE_HAPPENS_BEFORE(&b->sighandlers[sig]);
#endif
    if((sig < MAX_SIGNAL) &&
      atomic_compare_exchange_strong_explicit(&b->sighandlers[sig], &prev, NULL,
      memory_order_release, memory_order_relaxed))
    {
      struct sigaction new_action;

#if !defined(USE_SCHEDULER_SCALING_PTHREADS)
      // Make sure we ignore signals related to scheduler sleeping/waking
      // as the default for those signals is termination
      if(sig == PONY_SCHED_SLEEP_WAKE_SIGNAL)
        new_action.sa_handler = empty_signal_handler;
      else
#endif
        new_action.sa_handler = SIG_DFL;

      sigemptyset (&new_action.sa_mask);

      // ask to restart interrupted syscalls to match `signal` behavior
      new_action.sa_flags = SA_RESTART;

      sigaction(sig, &new_action, NULL);

      close(ev->asio_event_attr.fd);
      ev->asio_event_attr.fd = -1;
    }
  }

  ev->flags = ASIO_DISPOSABLE;
  send_request(ev, ASIO_DISPOSABLE);
}

#endif
