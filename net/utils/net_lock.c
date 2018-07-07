/****************************************************************************
 * net/utils/net_lock.c
 *
 *   Copyright (C) 2011-2012, 2014-2018 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <unistd.h>
#include <semaphore.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/semaphore.h>
#include <nuttx/mm/iob.h>
#include <nuttx/net/net.h>

#include "utils/utils.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NO_HOLDER (pid_t)-1

/****************************************************************************
 * Private Data
 ****************************************************************************/

static sem_t        g_netlock;
static pid_t        g_holder  = NO_HOLDER;
static unsigned int g_count   = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: _net_takesem
 *
 * Description:
 *   Take the semaphore, waiting indefinitely.
 *   REVISIT: Should this return if -EINTR?
 *
 ****************************************************************************/

static void _net_takesem(void)
{
  int ret;

  do
    {
      /* Take the semaphore (perhaps waiting) */

      ret = nxsem_wait(&g_netlock);

      /* The only case that an error should occur here is if the wait was
       * awakened by a signal.
       */

      DEBUGASSERT(ret == OK || ret == -EINTR);
    }
  while (ret == -EINTR);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_lockinitialize
 *
 * Description:
 *   Initialize the locking facility
 *
 ****************************************************************************/

void net_lockinitialize(void)
{
  nxsem_init(&g_netlock, 0, 1);
}

/****************************************************************************
 * Name: net_lock
 *
 * Description:
 *   Take the network lock
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void net_lock(void)
{
#ifdef CONFIG_SMP
  irqstate_t flags = enter_critical_section();
#endif
  pid_t me = getpid();

  /* Does this thread already hold the semaphore? */

  if (g_holder == me)
    {
      /* Yes.. just increment the reference count */

      g_count++;
    }
  else
    {
      /* No.. take the semaphore (perhaps waiting) */

      _net_takesem();

      /* Now this thread holds the semaphore */

      g_holder = me;
      g_count  = 1;
    }

#ifdef CONFIG_SMP
  leave_critical_section(flags);
#endif
}

/****************************************************************************
 * Name: net_unlock
 *
 * Description:
 *   Release the network lock.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void net_unlock(void)
{
#ifdef CONFIG_SMP
  irqstate_t flags = enter_critical_section();
#endif
  DEBUGASSERT(g_holder == getpid() && g_count > 0);

  /* If the count would go to zero, then release the semaphore */

  if (g_count == 1)
    {
      /* We no longer hold the semaphore */

      g_holder = NO_HOLDER;
      g_count  = 0;
      nxsem_post(&g_netlock);
    }
  else
    {
      /* We still hold the semaphore. Just decrement the count */

      g_count--;
    }

#ifdef CONFIG_SMP
  leave_critical_section(flags);
#endif
}

/****************************************************************************
 * Name: net_breaklock
 *
 * Description:
 *   Break the lock, return information needed to restore re-entrant lock
 *   state.
 *
 ****************************************************************************/

int net_breaklock(FAR unsigned int *count)
{
  irqstate_t flags;
  pid_t me = getpid();
  int ret = -EPERM;

  DEBUGASSERT(count != NULL);

  flags = spin_lock_irqsave(); /* No interrupts */
  if (g_holder == me)
    {
      /* Return the lock setting */

      *count   = g_count;

      /* Release the network lock  */

      g_holder = NO_HOLDER;
      g_count  = 0;

      (void)nxsem_post(&g_netlock);
      ret      = OK;
    }

  spin_unlock_irqrestore(flags);
  return ret;
}

/****************************************************************************
 * Name: net_breaklock
 *
 * Description:
 *   Restore the locked state
 *
 ****************************************************************************/

void net_restorelock(unsigned int count)
{
  pid_t me = getpid();

  DEBUGASSERT(g_holder != me);

  /* Recover the network lock at the proper count */

  _net_takesem();
  g_holder = me;
  g_count  = count;
}

/****************************************************************************
 * Name: net_timedwait
 *
 * Description:
 *   Atomically wait for sem (or a timeout( while temporarily releasing
 *   the lock on the network.
 *
 * Input Parameters:
 *   sem     - A reference to the semaphore to be taken.
 *   abstime - The absolute time to wait until a timeout is declared.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int net_timedwait(sem_t *sem, FAR const struct timespec *abstime)
{
  unsigned int count;
  irqstate_t   flags;
  int          blresult;
  int          ret;

  flags = enter_critical_section(); /* No interrupts */
  sched_lock();                     /* No context switches */

  /* Release the network lock, remembering my count.  net_breaklock will
   * return a negated value if the caller does not hold the network lock.
   */

  blresult = net_breaklock(&count);

  /* Now take the semaphore, waiting if so requested. */

  if (abstime != NULL)
    {
      /* Wait until we get the lock or until the timeout expires */

      ret = nxsem_timedwait(sem, abstime);
    }
  else
    {
      /* Wait as long as necessary to get the lock */

      ret = nxsem_wait(sem);
    }

  /* Recover the network lock at the proper count (if we held it before) */

  if (blresult >= 0)
    {
      net_restorelock(count);
    }

  sched_unlock();
  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: net_lockedwait
 *
 * Description:
 *   Atomically wait for sem while temporarily releasing the network lock.
 *
 * Input Parameters:
 *   sem - A reference to the semaphore to be taken.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int net_lockedwait(sem_t *sem)
{
  return net_timedwait(sem, NULL);
}

/****************************************************************************
 * Name: net_ioballoc
 *
 * Description:
 *   Allocate an IOB.  If no IOBs are available, then atomically wait for
 *   for the IOB while temporarily releasing the lock on the network.
 *
 * Input Parameters:
 *   throttled - An indication of the IOB allocation is "throttled"
 *
 * Returned Value:
 *   A pointer to the newly allocated IOB is returned on success.  NULL is
 *   returned on any allocation failure.
 *
 ****************************************************************************/

#ifdef CONFIG_MM_IOB
FAR struct iob_s *net_ioballoc(bool throttled)
{
  FAR struct iob_s *iob;

  iob = iob_tryalloc(throttled);
  if (iob == NULL)
    {
      irqstate_t flags;
      unsigned int count;
      int blresult;

      /* There are no buffers available now.  We will have to wait for one to
       * become available. But let's not do that with the network locked.
       */

      flags    = enter_critical_section();
      blresult = net_breaklock(&count);
      iob      = iob_alloc(throttled);
      if (blresult >= 0)
        {
          net_restorelock(count);
        }

      leave_critical_section(flags);
    }

  return iob;
}
#endif
