/****************************************************************************
*
*    Copyright (c) 2005 - 2012 by Vivante Corp.
*    
*    This program is free software; you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation; either version 2 of the license, or
*    (at your option) any later version.
*    
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*    GNU General Public License for more details.
*    
*    You should have received a copy of the GNU General Public License
*    along with this program; if not write to the Free Software
*    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*
*****************************************************************************/




#include "gc_hal_kernel_precomp.h"
#include "gc_hal_kernel_context.h"
#include <linux/kernel.h>

#ifdef __QNXNTO__
#include <sys/slog.h>
#endif

#define _GC_OBJ_ZONE            gcvZONE_COMMAND

/******************************************************************************\
********************************* Support Code *********************************
\******************************************************************************/

/*******************************************************************************
**
**  _NewQueue
**
**  Allocate a new command queue.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object.
**
**  OUTPUT:
**
**      gckCOMMAND Command
**          gckCOMMAND object has been updated with a new command queue.
*/
static gceSTATUS
_NewQueue(
    IN OUT gckCOMMAND Command
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctINT currentIndex, newIndex;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Switch to the next command buffer. */
    currentIndex = Command->index;
    newIndex     = (currentIndex + 1) % gcdCOMMAND_QUEUES;

    /* Wait for availability. */
#if gcdDUMP_COMMAND
    gcmkPRINT("@[kernel.waitsignal]");
#endif

    gcmkONERROR(gckOS_WaitSignal(
        Command->os,
        Command->queues[newIndex].signal,
        gcvINFINITE
        ));

#if gcmIS_DEBUG(gcdDEBUG_TRACE)
    if (newIndex < currentIndex)
    {
        Command->wrapCount += 1;

        gcmkTRACE_ZONE_N(
            gcvLEVEL_INFO, gcvZONE_COMMAND,
            2 * 4,
            "%s(%d): queue array wrapped around.\n",
            __FUNCTION__, __LINE__
            );
    }

    gcmkTRACE_ZONE_N(
        gcvLEVEL_INFO, gcvZONE_COMMAND,
        3 * 4,
        "%s(%d): total queue wrap arounds %d.\n",
        __FUNCTION__, __LINE__, Command->wrapCount
        );

    gcmkTRACE_ZONE_N(
        gcvLEVEL_INFO, gcvZONE_COMMAND,
        3 * 4,
        "%s(%d): switched to queue %d.\n",
        __FUNCTION__, __LINE__, newIndex
        );
#endif

    /* Update gckCOMMAND object with new command queue. */
    Command->index    = newIndex;
    Command->newQueue = gcvTRUE;
    Command->logical  = Command->queues[newIndex].logical;
    Command->offset   = 0;

    gcmkONERROR(gckOS_GetPhysicalAddressByHandle(
                        Command->os,
                        Command->logical,
                        Command->queues[newIndex].physical,
                        (gctUINT32 *) &Command->physical));

    gcmkONERROR(gckOS_SetGCPhysByMdl(Command->os, Command->queues[newIndex].physical, (gctUINT32)Command->physical));

    if (currentIndex != -1)
    {
        /* Mark the command queue as available. */
        gcmkONERROR(gckEVENT_Signal(
            Command->kernel->eventObj,
            Command->queues[currentIndex].signal,
            gcvKERNEL_COMMAND
            ));
    }

    /* Success. */
    gcmkFOOTER_ARG("Command->index=%d", Command->index);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

static gceSTATUS
_IncrementCommitAtom(
    IN gckCOMMAND Command,
    IN gctBOOL Increment
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gckHARDWARE hardware;
    gctINT32 atomValue;
    gctBOOL powerAcquired = gcvFALSE;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Extract the gckHARDWARE and gckEVENT objects. */
    hardware = Command->kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    /* Grab the power mutex. */
    gcmkONERROR(gckOS_AcquireRecMutex(
        Command->os, hardware->recMutexPower, gcvINFINITE
        ));
    powerAcquired = gcvTRUE;

    /* Increment the commit atom. */
    if (Increment)
    {
        gcmkONERROR(gckOS_AtomIncrement(
            Command->os, Command->atomCommit, &atomValue
            ));
    }
    else
    {
        gcmkONERROR(gckOS_AtomDecrement(
            Command->os, Command->atomCommit, &atomValue
            ));
    }

    /* Release the power mutex. */
    gcmkONERROR(gckOS_ReleaseRecMutex(
        Command->os, hardware->recMutexPower
        ));
    powerAcquired = gcvFALSE;

    /* Success. */
    gcmkFOOTER();
    return gcvSTATUS_OK;

OnError:
    if (powerAcquired)
    {
        /* Release the power mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseRecMutex(
            Command->os, hardware->recMutexPower
            ));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

#if gcdSECURE_USER
static gceSTATUS
_ProcessHints(
    IN gckCOMMAND Command,
    IN gctUINT32 ProcessID,
    IN gcoCMDBUF CommandBuffer
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gckKERNEL kernel;
    gctBOOL needCopy = gcvFALSE;
    gcskSECURE_CACHE_PTR cache;
    gctUINT8_PTR commandBufferLogical;
    gctUINT8_PTR hintedData;
    gctUINT32_PTR hintArray;
    gctUINT i, hintCount;

    gcmkHEADER_ARG(
        "Command=0x%08X ProcessID=%d CommandBuffer=0x%08X",
        Command, ProcessID, CommandBuffer
        );

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Reset state array pointer. */
    hintArray = gcvNULL;

    /* Get the kernel object. */
    kernel = Command->kernel;

    /* Get the cache form the database. */
    gcmkONERROR(gckKERNEL_GetProcessDBCache(kernel, ProcessID, &cache));

    /* Determine the start of the command buffer. */
    commandBufferLogical
        = (gctUINT8_PTR) CommandBuffer->logical
        +                CommandBuffer->startOffset;

    /* Determine the number of records in the state array. */
    hintCount = CommandBuffer->hintArrayTail - CommandBuffer->hintArray;

    /* Check wehther we need to copy the structures or not. */
    gcmkONERROR(gckOS_QueryNeedCopy(Command->os, ProcessID, &needCopy));

    /* Get access to the state array. */
    if (needCopy)
    {
        gctUINT copySize;

        if (Command->hintArrayAllocated &&
            (Command->hintArraySize < CommandBuffer->hintArraySize))
        {
            gcmkONERROR(gcmkOS_SAFE_FREE(Command->os, Command->hintArray));
            Command->hintArraySize = gcvFALSE;
        }

        if (!Command->hintArrayAllocated)
        {
            gctPOINTER pointer = gcvNULL;

            gcmkONERROR(gckOS_Allocate(
                Command->os,
                CommandBuffer->hintArraySize,
                &pointer
                ));

            Command->hintArray          = pointer;
            Command->hintArrayAllocated = gcvTRUE;
            Command->hintArraySize      = CommandBuffer->hintArraySize;
        }

        hintArray = Command->hintArray;
        copySize   = hintCount * gcmSIZEOF(gctUINT32);

        gcmkONERROR(gckOS_CopyFromUserData(
            Command->os,
            hintArray,
            CommandBuffer->hintArray,
            copySize
            ));
    }
    else
    {
        gctPOINTER pointer = gcvNULL;

        gcmkONERROR(gckOS_MapUserPointer(
            Command->os,
            CommandBuffer->hintArray,
            CommandBuffer->hintArraySize,
            &pointer
            ));

        hintArray = pointer;
    }

    /* Scan through the buffer. */
    for (i = 0; i < hintCount; i += 1)
    {
        /* Determine the location of the hinted data. */
        hintedData = commandBufferLogical + hintArray[i];

        /* Map handle into physical address. */
        gcmkONERROR(gckKERNEL_MapLogicalToPhysical(
            kernel, cache, (gctPOINTER) hintedData
            ));
    }

OnError:
    /* Get access to the state array. */
    if (!needCopy && (hintArray != gcvNULL))
    {
        gcmkVERIFY_OK(gckOS_UnmapUserPointer(
            Command->os,
            CommandBuffer->hintArray,
            CommandBuffer->hintArraySize,
            hintArray
            ));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}
#endif
#if !gcdFLUSH_FIX
static gceSTATUS
_FlushMMU(
    IN gckCOMMAND Command
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctUINT32 oldValue;
    gckHARDWARE hardware = Command->kernel->hardware;

    gcmkONERROR(gckOS_AtomicExchange(Command->os,
                                     hardware->pageTableDirty,
                                     0,
                                     &oldValue));

    if (oldValue)
    {
        /* Page Table is upated, flush mmu before commit. */
        gcmkONERROR(gckHARDWARE_FlushMMU(hardware));
    }

    return gcvSTATUS_OK;
OnError:
    return status;
}
#endif
/******************************************************************************\
****************************** gckCOMMAND API Code ******************************
\******************************************************************************/

/*******************************************************************************
**
**  gckCOMMAND_Construct
**
**  Construct a new gckCOMMAND object.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**  OUTPUT:
**
**      gckCOMMAND * Command
**          Pointer to a variable that will hold the pointer to the gckCOMMAND
**          object.
*/
gceSTATUS
gckCOMMAND_Construct(
    IN gckKERNEL Kernel,
    OUT gckCOMMAND * Command
    )
{
    gckOS os;
    gckCOMMAND command = gcvNULL;
    gceSTATUS status = gcvSTATUS_OK;
    gctINT i;
    gctPOINTER pointer = gcvNULL;

    gcmkHEADER_ARG("Kernel=0x%x", Kernel);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Kernel, gcvOBJ_KERNEL);
    gcmkVERIFY_ARGUMENT(Command != gcvNULL);

    /* Extract the gckOS object. */
    os = Kernel->os;

    /* Allocate the gckCOMMAND structure. */
    gcmkONERROR(gckOS_Allocate(os, gcmSIZEOF(struct _gckCOMMAND), &pointer));
    command = pointer;

    /* Reset the entire object. */
    gcmkONERROR(gckOS_ZeroMemory(command, gcmSIZEOF(struct _gckCOMMAND)));

    /* Initialize the gckCOMMAND object.*/
    command->object.type    = gcvOBJ_COMMAND;
    command->kernel         = Kernel;
    command->os             = os;

    /* Get the command buffer requirements. */
    gcmkONERROR(gckHARDWARE_QueryCommandBuffer(
        Kernel->hardware,
        &command->alignment,
        &command->reservedHead,
        &command->reservedTail
        ));

    /* Create the command queue mutex. */
    gcmkONERROR(gckOS_CreateMutex(os, &command->mutexQueue));

    /* Create the context switching mutex. */
    gcmkONERROR(gckOS_CreateMutex(os, &command->mutexContext));

    /* Create the power management semaphore. */
    gcmkONERROR(gckOS_CreateSemaphore(os, &command->powerSemaphore));

    /* Create the commit atom. */
    gcmkONERROR(gckOS_AtomConstruct(os, &command->atomCommit));

    /* Get the page size from teh OS. */
    gcmkONERROR(gckOS_GetPageSize(os, &command->pageSize));

    /* Get process ID. */
    gcmkONERROR(gckOS_GetProcessID(&command->kernelProcessID));

    /* Set hardware to pipe 0. */
    command->pipeSelect = gcvPIPE_INVALID;

    /* Pre-allocate the command queues. */
    for (i = 0; i < gcdCOMMAND_QUEUES; ++i)
    {
        gcmkONERROR(gckOS_AllocateNonPagedMemory(
            os,
            gcvFALSE,
            &command->pageSize,
            &command->queues[i].physical,
            &command->queues[i].logical
            ));

        gcmkONERROR(gckOS_CreateSignal(
            os, gcvFALSE, &command->queues[i].signal
            ));

        gcmkONERROR(gckOS_Signal(
            os, command->queues[i].signal, gcvTRUE
            ));
    }

    /* No command queue in use yet. */
    command->index    = -1;
    command->logical  = gcvNULL;
    command->newQueue = gcvFALSE;

    /* Command is not yet running. */
    command->running = gcvFALSE;

    /* Command queue is idle. */
    command->idle = gcvTRUE;

    /* Commit stamp is zero. */
    command->commitStamp = 0;

    /* END event signal not created. */
    command->endEventSignal = gcvNULL;

    command->queue.front = 0;
    command->queue.rear = 0;
    command->queue.count = 0;
    command->queue.init = gcvFALSE;

    /* Return pointer to the gckCOMMAND object. */
    *Command = command;

    /* Success. */
    gcmkFOOTER_ARG("*Command=0x%x", *Command);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    if (command != gcvNULL)
    {
        if (command->atomCommit != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_AtomDestroy(os, command->atomCommit));
        }

        if (command->powerSemaphore != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_DestroySemaphore(os, command->powerSemaphore));
        }

        if (command->mutexContext != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_DeleteMutex(os, command->mutexContext));
        }

        if (command->mutexQueue != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_DeleteMutex(os, command->mutexQueue));
        }

        for (i = 0; i < gcdCOMMAND_QUEUES; ++i)
        {
            if (command->queues[i].signal != gcvNULL)
            {
                gcmkVERIFY_OK(gckOS_DestroySignal(
                    os, command->queues[i].signal
                    ));
            }

            if (command->queues[i].logical != gcvNULL)
            {
                gcmkVERIFY_OK(gckOS_FreeNonPagedMemory(
                    os,
                    command->pageSize,
                    command->queues[i].physical,
                    command->queues[i].logical
                    ));
            }
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, command));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Destroy
**
**  Destroy an gckCOMMAND object.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object to destroy.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Destroy(
    IN gckCOMMAND Command
    )
{
    gctINT i;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Stop the command queue. */
    gcmkVERIFY_OK(gckCOMMAND_Stop(Command, gcvFALSE));

    for (i = 0; i < gcdCOMMAND_QUEUES; ++i)
    {
        gcmkASSERT(Command->queues[i].signal != gcvNULL);
        gcmkVERIFY_OK(gckOS_DestroySignal(
            Command->os, Command->queues[i].signal
            ));

        gcmkASSERT(Command->queues[i].logical != gcvNULL);
        gcmkVERIFY_OK(gckOS_FreeNonPagedMemory(
            Command->os,
            Command->pageSize,
            Command->queues[i].physical,
            Command->queues[i].logical
            ));
    }

    /* END event signal. */
    if (Command->endEventSignal != gcvNULL)
    {
        gcmkVERIFY_OK(gckOS_DestroySignal(
            Command->os, Command->endEventSignal
            ));
    }

    /* Delete the context switching mutex. */
    gcmkVERIFY_OK(gckOS_DeleteMutex(Command->os, Command->mutexContext));

    /* Delete the command queue mutex. */
    gcmkVERIFY_OK(gckOS_DeleteMutex(Command->os, Command->mutexQueue));

    /* Destroy the power management semaphore. */
    gcmkVERIFY_OK(gckOS_DestroySemaphore(Command->os, Command->powerSemaphore));

    /* Destroy the commit atom. */
    gcmkVERIFY_OK(gckOS_AtomDestroy(Command->os, Command->atomCommit));

#if gcdSECURE_USER
    /* Free state array. */
    if (Command->hintArrayAllocated)
    {
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Command->os, Command->hintArray));
        Command->hintArrayAllocated = gcvFALSE;
    }
#endif

    /* Mark object as unknown. */
    Command->object.type = gcvOBJ_UNKNOWN;

    /* Free the gckCOMMAND object. */
    gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Command->os, Command));

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckCOMMAND_EnterCommit
**
**  Acquire command queue synchronization objects.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object to destroy.
**
**      gctBOOL FromPower
**          Determines whether the call originates from inside the power
**          management or not.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_EnterCommit(
    IN gckCOMMAND Command,
    IN gctBOOL FromPower
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gckHARDWARE hardware;
    gctBOOL atomIncremented = gcvFALSE;
    gctBOOL semaAcquired = gcvFALSE;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Extract the gckHARDWARE and gckEVENT objects. */
    hardware = Command->kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    if (!FromPower)
    {
        /* Increment COMMIT atom to let power management know that a commit is
        ** in progress. */
        gcmkONERROR(_IncrementCommitAtom(Command, gcvTRUE));
        atomIncremented = gcvTRUE;

        /* Notify the system the GPU has a commit. */
        gcmkONERROR(gckOS_Broadcast(Command->os,
                                    hardware,
                                    gcvBROADCAST_GPU_COMMIT));

        /* Acquire the power management semaphore. */
        gcmkONERROR(gckOS_AcquireSemaphore_Timeout(Command->os,
                                                   Command->powerSemaphore,
                                                   (gcdGPU_ADVANCETIMER_STALL*25)));
        semaAcquired = gcvTRUE;
    }

    /* Grab the conmmand queue mutex. */
    gcmkONERROR(gckOS_AcquireMutex(Command->os,
                                   Command->mutexQueue,
                                   gcvINFINITE));

    /* Success. */
    gcmkFOOTER();
    return gcvSTATUS_OK;

OnError:
    if (semaAcquired)
    {
        /* Release the power management semaphore. */
        gcmkVERIFY_OK(gckOS_ReleaseSemaphore(
            Command->os, Command->powerSemaphore
            ));
    }

    if (atomIncremented)
    {
        /* Decrement the commit atom. */
        gcmkVERIFY_OK(_IncrementCommitAtom(
            Command, gcvFALSE
            ));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_ExitCommit
**
**  Release command queue synchronization objects.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object to destroy.
**
**      gctBOOL FromPower
**          Determines whether the call originates from inside the power
**          management or not.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_ExitCommit(
    IN gckCOMMAND Command,
    IN gctBOOL FromPower
    )
{
    gceSTATUS status = gcvSTATUS_OK;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Release the power mutex. */
    gcmkONERROR(gckOS_ReleaseMutex(Command->os, Command->mutexQueue));

    if (!FromPower)
    {
        /* Release the power management semaphore. */
        gcmkONERROR(gckOS_ReleaseSemaphore(Command->os,
                                           Command->powerSemaphore));

        /* Decrement the commit atom. */
        gcmkONERROR(_IncrementCommitAtom(Command, gcvFALSE));
    }

    /* Success. */
    gcmkFOOTER();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Start
**
**  Start up the command queue.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object to start.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Start(
    IN gckCOMMAND Command
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gckHARDWARE hardware;
    gctUINT32 waitOffset;
    gctSIZE_T waitLinkBytes;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    if (Command->running)
    {
        /* Command queue already running. */
        gcmkFOOTER_NO();
        return gcvSTATUS_OK;
    }

    /* Extract the gckHARDWARE object. */
    hardware = Command->kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    if (Command->logical == gcvNULL)
    {
        /* Start at beginning of a new queue. */
        gcmkONERROR(_NewQueue(Command));
    }

    /* Start at beginning of page. */
    Command->offset = 0;

    /* Set abvailable number of bytes for WAIT/LINK command sequence. */
    waitLinkBytes = Command->pageSize;

    /* Append WAIT/LINK. */
    gcmkONERROR(gckHARDWARE_WaitLink(
        hardware,
        Command->logical,
        Command->logical,
        0,
        &waitLinkBytes,
        &waitOffset,
        &Command->waitSize,
        Command->queues[Command->index].physical
        ));

    Command->waitLogical  = (gctUINT8_PTR) Command->logical  + waitOffset;
    Command->waitPhysical = (gctUINT8_PTR) Command->physical + waitOffset;

#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the cache for the wait/link. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        Command->kernelProcessID,
        gcvNULL,
        Command->physical,
        Command->logical,
        waitLinkBytes
        ));
#endif

    /* Adjust offset. */
    Command->offset   = waitLinkBytes;
    Command->newQueue = gcvFALSE;

    /* Enable command processor. */
#ifdef __QNXNTO__
    gcmkONERROR(gckHARDWARE_Execute(
        hardware,
        Command->logical,
        Command->physical,
        gcvTRUE,
        waitLinkBytes
        ));
#else
    gcmkONERROR(gckHARDWARE_Execute(
        hardware,
        Command->logical,
        Command->logical,
        waitLinkBytes,
        Command->queues[Command->index].physical
        ));
#endif

    /* Command queue is running. */
    Command->running = gcvTRUE;

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Stop
**
**  Stop the command queue.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object to stop.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Stop(
    IN gckCOMMAND Command,
    IN gctBOOL FromRecovery
    )
{
    gckHARDWARE hardware;
    gceSTATUS status = gcvSTATUS_OK;
    gctUINT32 idle;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    if (!Command->running)
    {
        /* Command queue is not running. */
        gcmkFOOTER_NO();
        return gcvSTATUS_OK;
    }

    /* Extract the gckHARDWARE object. */
    hardware = Command->kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    if (gckHARDWARE_IsFeatureAvailable(hardware,
                                       gcvFEATURE_END_EVENT) == gcvSTATUS_TRUE)
    {
        /* Allocate the signal. */
        if (Command->endEventSignal == gcvNULL)
        {
            gcmkONERROR(gckOS_CreateSignal(Command->os,
                                           gcvTRUE,
                                           &Command->endEventSignal));
        }

        /* Append the END EVENT command to trigger the signal. */
        gcmkONERROR(gckEVENT_Stop(Command->kernel->eventObj,
                                  Command->kernelProcessID,
                                  Command->waitPhysical,
                                  Command->waitLogical,
                                  Command->endEventSignal,
                                  &Command->waitSize));
    }
    else
    {
        /* Replace last WAIT with END. */
        gcmkONERROR(gckHARDWARE_End(
            hardware, Command->waitLogical, &Command->waitSize
            ));

        /* Update queue tail pointer. */
        gcmkONERROR(gckHARDWARE_UpdateQueueTail(Command->kernel->hardware,
                                                Command->logical,
                                                Command->offset));

#if gcdNONPAGED_MEMORY_CACHEABLE
        /* Flush the cache for the END. */
        gcmkONERROR(gckOS_CacheClean(
            Command->os,
            Command->kernelProcessID,
            gcvNULL,
            Command->waitPhysical,
            Command->waitLogical,
            Command->waitSize
            ));
#endif

        /* Wait for idle. */
        gcmkONERROR(gckHARDWARE_GetIdle(hardware, !FromRecovery, &idle));
    }

    /* Command queue is no longer running. */
    Command->running = gcvFALSE;

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Commit
**
**  Commit a command buffer to the command queue.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to a gckCOMMAND object.
**
**      gckCONTEXT Context
**          Pointer to a gckCONTEXT object.
**
**      gcoCMDBUF CommandBuffer
**          Pointer to a gcoCMDBUF object.
**
**      gcsSTATE_DELTA_PTR StateDelta
**          Pointer to the state delta.
**
**      gctUINT32 ProcessID
**          Current process ID.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Commit(
    IN gckCOMMAND Command,
    IN gckCONTEXT Context,
    IN gcoCMDBUF CommandBuffer,
    IN gcsSTATE_DELTA_PTR StateDelta,
    IN gcsQUEUE_PTR EventQueue,
    IN gctUINT32 ProcessID
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctBOOL commitEntered = gcvFALSE;
    gctBOOL contextAcquired = gcvFALSE;
    gckHARDWARE hardware;
    gctBOOL needCopy = gcvFALSE;
    gcsQUEUE_PTR eventRecord = gcvNULL;
    gcsQUEUE _eventRecord;
    gcsQUEUE_PTR nextEventRecord;
    gctBOOL commandBufferMapped = gcvFALSE;
    gcoCMDBUF commandBufferObject = gcvNULL;

#if !gcdNULL_DRIVER
    gcsCONTEXT_PTR contextBuffer = gcvNULL;
    struct _gcoCMDBUF _commandBufferObject;
    gctPHYS_ADDR commandBufferPhysical;
    gctPHYS_ADDR commandBufferHandle;
    gctUINT8_PTR commandBufferLogical;
    gctUINT8_PTR commandBufferLink;
    gctUINT commandBufferSize;
    gctSIZE_T nopBytes;
    gctSIZE_T pipeBytes;
    gctSIZE_T linkBytes;
    gctSIZE_T bytes;
    gctUINT32 offset;
#if gcdNONPAGED_MEMORY_CACHEABLE
    gctPHYS_ADDR entryPhysical;
#endif
    gctPOINTER entryLogical;
    gctPOINTER entryLogicalBase;
    gctPHYS_ADDR entryMdlHandle;
    gctSIZE_T entryBytes;
#if gcdNONPAGED_MEMORY_CACHEABLE
    gctPHYS_ADDR exitPhysical;
#endif
    gctPOINTER exitLogical;
    gctPOINTER exitLogicalBase;
    gctPOINTER exitMdlHandle;

    gctSIZE_T exitBytes;
    gctPHYS_ADDR waitLinkPhysical;
    gctPOINTER waitLinkLogical;
    gctPOINTER waitLinkLogicalBase = gcvNULL;
    gctPHYS_ADDR waitLinkMdlHandle = gcvNULL;
    gctSIZE_T waitLinkBytes;
    gctPHYS_ADDR waitPhysical;
    gctPOINTER waitLogical;
    gctUINT32 waitOffset;
    gctSIZE_T waitSize;

#if gcdFLUSH_FIX
    gctSIZE_T mmuConfigureBytes;
    gctPOINTER mmuConfigureLogical = gcvNULL;
    gctPOINTER mmuConfigurePhysical = 0;
    gctSIZE_T mmuConfigureWaitLinkOffset;
    gctSIZE_T reservedBytes;
    gctUINT32 oldValue = 0;
#endif

#if gcdDUMP_COMMAND
    gctPOINTER contextDumpLogical = gcvNULL;
    gctSIZE_T contextDumpBytes = 0;
    gctPOINTER bufferDumpLogical = gcvNULL;
    gctSIZE_T bufferDumpBytes = 0;
# endif
#endif

    gctPOINTER pointer = gcvNULL;

    gcmkHEADER_ARG(
        "Command=0x%x CommandBuffer=0x%x ProcessID=%d",
        Command, CommandBuffer, ProcessID
        );

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    if (Command->kernel->core == gcvCORE_2D)
    {
        /* There is no context for 2D. */
        Context = gcvNULL;
    }

#if !gcdFLUSH_FIX
    gcmkONERROR(_FlushMMU(Command));
#endif

    /* Acquire the command queue. */
    gcmkONERROR(gckCOMMAND_EnterCommit(Command, gcvFALSE));
    commitEntered = gcvTRUE;

#if gcdFLUSH_FIX
    gckOS_AtomGet(Command->os,
                  Command->kernel->hardware->pageTableDirty,
                  &oldValue
                  );
#endif

    /* Acquire the context switching mutex. */
    gcmkONERROR(gckOS_AcquireMutex(
        Command->os, Command->mutexContext, gcvINFINITE
        ));
    contextAcquired = gcvTRUE;

    /* Extract the gckHARDWARE and gckEVENT objects. */
    hardware = Command->kernel->hardware;

    /* Check wehther we need to copy the structures or not. */
    gcmkONERROR(gckOS_QueryNeedCopy(Command->os, ProcessID, &needCopy));

#if gcdNULL_DRIVER

    /* Context switch required? */
    if ((Context != gcvNULL) && (Command->currContext != Context))
    {
        /* Yes, merge in the deltas. */
        gckCONTEXT_Update(Context, ProcessID, StateDelta);

        /* Update the current context. */
        Command->currContext = Context;
    }
#else
    if (needCopy)
    {
        commandBufferObject = &_commandBufferObject;

        gcmkONERROR(gckOS_CopyFromUserData(
            Command->os,
            commandBufferObject,
            CommandBuffer,
            gcmSIZEOF(struct _gcoCMDBUF)
            ));

        gcmkVERIFY_OBJECT_NORETUNE(commandBufferObject, gcvOBJ_COMMANDBUFFER);
    }
    else
    {
        gcmkONERROR(gckOS_MapUserPointer(
            Command->os,
            CommandBuffer,
            gcmSIZEOF(struct _gcoCMDBUF),
            &pointer
            ));

        commandBufferObject = pointer;

        gcmkVERIFY_OBJECT_NORETUNE(commandBufferObject, gcvOBJ_COMMANDBUFFER);
        commandBufferMapped = gcvTRUE;
    }

    /* Query the size of NOP command. */
    gcmkONERROR(gckHARDWARE_Nop(
        hardware, gcvNULL, &nopBytes
        ));

    /* Query the size of pipe select command sequence. */
    gcmkONERROR(gckHARDWARE_PipeSelect(
        hardware, gcvNULL, gcvPIPE_3D, &pipeBytes
        ));

    /* Query the size of LINK command. */
    gcmkONERROR(gckHARDWARE_Link(
        hardware, gcvNULL, gcvNULL, gcvNULL, 0, &linkBytes, 0
        ));

    /* Compute the command buffer entry and the size. */
    commandBufferLogical
        = (gctUINT8_PTR) commandBufferObject->logical
        +                commandBufferObject->startOffset;


     /* shortcut to command buffer mdl handle */
    commandBufferHandle = (gctPHYS_ADDR)commandBufferObject->physical;

    gcmkONERROR(gckOS_GetGCPhysByMdl(Command->os,
                                commandBufferObject->logical,
                                commandBufferHandle,
                                (gctUINT32 *)&commandBufferPhysical));
    commandBufferSize
        = commandBufferObject->offset
        + Command->reservedTail
        - commandBufferObject->startOffset;

    /* Get the current offset. */
    offset = Command->offset;

    /* Compute number of bytes left in current kernel command queue. */
    bytes = Command->pageSize - offset;

#if gcdFLUSH_FIX
    if (oldValue)
    {
        /* Query the size of PAGE TABLE SWITCH command sequence. */
        gcmkONERROR(gckHARDWARE_ConfigMMU(
            hardware,
            gcvNULL,
            gcvNULL,
            offset,
            &mmuConfigureBytes,
            &mmuConfigureWaitLinkOffset,
            &waitLinkBytes
            ));

        /* Is there enough space in the current command queue? */
        if (bytes < mmuConfigureBytes)
        {
            /* No, create a new one. */
            gcmkONERROR(_NewQueue(Command));

            /* Get the new current offset. */
            offset = Command->offset;

            /* Recompute the number of bytes in the new kernel command queue. */
            bytes = Command->pageSize - offset;
            gcmkASSERT(bytes >= mmuConfigureBytes);
        }

        mmuConfigurePhysical = (gctUINT8_PTR) Command->physical + offset;
        mmuConfigureLogical  = (gctUINT8_PTR) Command->logical  + offset;

        /* Compute the location if WAIT/LINK command sequence. */
        waitLinkPhysical = (gctUINT8_PTR) mmuConfigurePhysical + mmuConfigureWaitLinkOffset;
        waitLinkLogical  = (gctUINT8_PTR) mmuConfigureLogical  + mmuConfigureWaitLinkOffset;

        waitLinkLogicalBase = (gctUINT8_PTR) Command->logical;
        waitLinkMdlHandle   =  Command->queues[Command->index].physical;

        reservedBytes = mmuConfigureBytes;
    }
    else
#endif
    {

    /* Query the size of WAIT/LINK command sequence. */
    gcmkONERROR(gckHARDWARE_WaitLink(
        hardware,
        gcvNULL,
        gcvNULL,
        offset,
        &waitLinkBytes,
        gcvNULL,
        gcvNULL,
        gcvNULL
        ));

    /* Is there enough space in the current command queue? */
    if (bytes < waitLinkBytes)
    {
        /* No, create a new one. */
        gcmkONERROR(_NewQueue(Command));

        /* Get the new current offset. */
        offset = Command->offset;

        /* Recompute the number of bytes in the new kernel command queue. */
        bytes = Command->pageSize - offset;
        gcmkASSERT(bytes >= waitLinkBytes);
    }

    /* Compute the location if WAIT/LINK command sequence. */
    waitLinkPhysical = (gctUINT8_PTR) Command->physical + offset;
    waitLinkLogical  = (gctUINT8_PTR) Command->logical  + offset;
    waitLinkLogicalBase = (gctUINT8_PTR) Command->logical;
    waitLinkMdlHandle   =  Command->queues[Command->index].physical;

#if gcdFLUSH_FIX
    reservedBytes = waitLinkBytes;
#endif
	}

    /* Context switch required? */
    if (Context == gcvNULL)
    {
        /* See if we have to switch pipes for the command buffer. */
        if (commandBufferObject->entryPipe == Command->pipeSelect)
        {
            /* Skip pipe switching sequence. */
            offset = pipeBytes;
        }
        else
        {
            /* The current hardware and the entry command buffer pipes
            ** are different, switch to the correct pipe. */
            gcmkONERROR(gckHARDWARE_PipeSelect(
                Command->kernel->hardware,
                commandBufferLogical,
                commandBufferObject->entryPipe,
                &pipeBytes
                ));

            /* Do not skip pipe switching sequence. */
            offset = 0;
        }

        /* Compute the entry. */
#if gcdNONPAGED_MEMORY_CACHEABLE
        entryPhysical = (gctUINT8_PTR) commandBufferPhysical + offset;
#endif
        entryLogical  =                commandBufferLogical  + offset;
        entryLogicalBase =             commandBufferObject->logical;
        entryMdlHandle =               commandBufferHandle;
        entryBytes    =                commandBufferSize     - offset;
    }
    else if (Command->currContext != Context)
    {
        /* Temporary disable context length oprimization. */
        Context->dirty = gcvTRUE;

        /* Get the current context buffer. */
        contextBuffer = Context->buffer;

        /* Yes, merge in the deltas. */
        gcmkONERROR(gckCONTEXT_Update(Context, ProcessID, StateDelta));

        /* Determine context entry and exit points. */
        if (0)
        {
            /* Reset 2D dirty flag. */
            Context->dirty2D = gcvFALSE;

            if (Context->dirty || commandBufferObject->using3D)
            {
                /***************************************************************
                ** SWITCHING CONTEXT: 2D and 3D are used.
                */

                /* Reset 3D dirty flag. */
                Context->dirty3D = gcvFALSE;

                /* Compute the entry. */
                if (Command->pipeSelect == gcvPIPE_2D)
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical + pipeBytes;
#endif
                    entryLogical     = (gctUINT8_PTR) contextBuffer->logical  + pipeBytes;
                    entryLogicalBase = (gctUINT8_PTR)contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->bufferSize     - pipeBytes;
                }
                else
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical;
#endif
                    entryLogical     = (gctUINT8_PTR) contextBuffer->logical;
                    entryLogicalBase = (gctUINT8_PTR)contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->bufferSize;
                }

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_3D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Ensure the NOP between 2D and 3D is in place so that the
                   execution falls through from 2D to 3D. */
                gcmkONERROR(gckHARDWARE_Nop(
                    hardware,
                    contextBuffer->link2D,
                    &nopBytes
                    ));

                /* Generate a LINK from the context buffer to
                   the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link3D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));

                /* Mark context as not dirty. */
                Context->dirty = gcvFALSE;
            }
            else
            {
                /***************************************************************
                ** SWITCHING CONTEXT: 2D only command buffer.
                */

                /* Mark 3D as dirty. */
                Context->dirty3D = gcvTRUE;

                /* Compute the entry. */
                if (Command->pipeSelect == gcvPIPE_2D)
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical + pipeBytes;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical  + pipeBytes;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->entryOffset3D  - pipeBytes;
                }
                else
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->entryOffset3D;
                }

                /* Store the current context buffer. */
                Context->dirtyBuffer = contextBuffer;

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_2D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* 3D is not used, generate a LINK from the end of 2D part of
                   the context buffer to the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link2D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
        }

        /* Not using 2D. */
        else
        {
            /* Mark 2D as dirty. */
            Context->dirty2D = gcvTRUE;

            /* Store the current context buffer. */
            Context->dirtyBuffer = contextBuffer;

            if (Context->dirty || commandBufferObject->using3D)
            {
                /***************************************************************
                ** SWITCHING CONTEXT: 3D only command buffer.
                */

                /* Reset 3D dirty flag. */
                Context->dirty3D = gcvFALSE;

                /* Determine context buffer entry offset. */
                offset = (Command->pipeSelect == gcvPIPE_3D)

                    /* Skip pipe switching sequence. */
                    ? Context->entryOffset3D + pipeBytes

                    /* Do not skip pipe switching sequence. */
                    : Context->entryOffset3D;

                /* Compute the entry. */
#if gcdNONPAGED_MEMORY_CACHEABLE
                entryPhysical = (gctUINT8_PTR) contextBuffer->physical + offset;
#endif

                entryLogical  = (gctUINT8_PTR) contextBuffer->logical  + offset;
                entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                entryMdlHandle   =  contextBuffer->physical;
                entryBytes       =  Context->bufferSize     - offset;

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_3D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Generate a LINK from the context buffer to
                   the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link3D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
            else
            {
                /***************************************************************
                ** SWITCHING CONTEXT: "XD" command buffer - neither 2D nor 3D.
                */

                /* Mark 3D as dirty. */
                Context->dirty3D = gcvTRUE;

                /* Compute the entry. */
                if (Command->pipeSelect == gcvPIPE_3D)
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical
                        = (gctUINT8_PTR) contextBuffer->physical
                        + Context->entryOffsetXDFrom3D;
#endif
                    entryLogical
                        = (gctUINT8_PTR) contextBuffer->logical
                        + Context->entryOffsetXDFrom3D;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   = contextBuffer->physical;

                    entryBytes
                        = Context->bufferSize
                        - Context->entryOffsetXDFrom3D;
                }
                else
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical
                        = (gctUINT8_PTR) contextBuffer->physical
                        + Context->entryOffsetXDFrom2D;
#endif
                    entryLogical
                        = (gctUINT8_PTR) contextBuffer->logical
                        + Context->entryOffsetXDFrom2D;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   = contextBuffer->physical;

                    entryBytes
                        = Context->totalSize
                        - Context->entryOffsetXDFrom2D;
                }

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_3D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Generate a LINK from the context buffer to
                   the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link3D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
        }

#if gcdNONPAGED_MEMORY_CACHEABLE
        /* Flush the context buffer cache. */
        gcmkONERROR(gckOS_CacheClean(
            Command->os,
            Command->kernelProcessID,
            gcvNULL,
            entryPhysical,
            entryLogical,
            entryBytes
            ));
#endif

        /* Update the current context. */
        Command->currContext = Context;

#if gcdDUMP_COMMAND
        contextDumpLogical = entryLogical;
        contextDumpBytes   = entryBytes;
#endif
    }

    /* Same context. */
    else
    {
        /* Determine context entry and exit points. */
        if (commandBufferObject->using2D && Context->dirty2D)
        {
            /* Reset 2D dirty flag. */
            Context->dirty2D = gcvFALSE;

            /* Get the "dirty" context buffer. */
            contextBuffer = Context->dirtyBuffer;

            if (commandBufferObject->using3D && Context->dirty3D)
            {
                /* Reset 3D dirty flag. */
                Context->dirty3D = gcvFALSE;

                /* Compute the entry. */
                if (Command->pipeSelect == gcvPIPE_2D)
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical + pipeBytes;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical  + pipeBytes;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->bufferSize     - pipeBytes;
                }
                else
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->bufferSize;
                }

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_3D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Ensure the NOP between 2D and 3D is in place so that the
                   execution falls through from 2D to 3D. */
                gcmkONERROR(gckHARDWARE_Nop(
                    hardware,
                    contextBuffer->link2D,
                    &nopBytes
                    ));

                /* Generate a LINK from the context buffer to
                   the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link3D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
            else
            {
                /* Compute the entry. */
                if (Command->pipeSelect == gcvPIPE_2D)
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical + pipeBytes;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical  + pipeBytes;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   =  contextBuffer->physical;
                    entryBytes       =  Context->entryOffset3D  - pipeBytes;
                }
                else
                {
#if gcdNONPAGED_MEMORY_CACHEABLE
                    entryPhysical = (gctUINT8_PTR) contextBuffer->physical;
#endif
                    entryLogical  = (gctUINT8_PTR) contextBuffer->logical;
                    entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                    entryMdlHandle   = contextBuffer->physical;
                    entryBytes       = Context->entryOffset3D;
                }

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_2D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* 3D is not used, generate a LINK from the end of 2D part of
                   the context buffer to the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link2D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
        }
        else
        {
            if (commandBufferObject->using3D && Context->dirty3D)
            {
                /* Reset 3D dirty flag. */
                Context->dirty3D = gcvFALSE;

                /* Get the "dirty" context buffer. */
                contextBuffer = Context->dirtyBuffer;

                /* Determine context buffer entry offset. */
                offset = (Command->pipeSelect == gcvPIPE_3D)

                    /* Skip pipe switching sequence. */
                    ? Context->entryOffset3D + pipeBytes

                    /* Do not skip pipe switching sequence. */
                    : Context->entryOffset3D;

                /* Compute the entry. */
#if gcdNONPAGED_MEMORY_CACHEABLE
                entryPhysical = (gctUINT8_PTR) contextBuffer->physical + offset;
#endif
                entryLogical  = (gctUINT8_PTR) contextBuffer->logical  + offset;
                entryLogicalBase = (gctUINT8_PTR) contextBuffer->logical;
                entryMdlHandle   = contextBuffer->physical;
                entryBytes       = Context->bufferSize     - offset;

                /* See if we have to switch pipes between the context
                   and command buffers. */
                if (commandBufferObject->entryPipe == gcvPIPE_3D)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the initial context pipes are
                       different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Generate a LINK from the context buffer to
                   the command buffer. */
                gcmkONERROR(gckHARDWARE_Link(
                    hardware,
                    contextBuffer->link3D,
                    commandBufferObject->logical,
                    commandBufferLogical + offset,
                    commandBufferSize    - offset,
                    &linkBytes,
                    commandBufferHandle
                    ));
            }
            else
            {
                /* See if we have to switch pipes for the command buffer. */
                if (commandBufferObject->entryPipe == Command->pipeSelect)
                {
                    /* Skip pipe switching sequence. */
                    offset = pipeBytes;
                }
                else
                {
                    /* The current hardware and the entry command buffer pipes
                    ** are different, switch to the correct pipe. */
                    gcmkONERROR(gckHARDWARE_PipeSelect(
                        Command->kernel->hardware,
                        commandBufferLogical,
                        commandBufferObject->entryPipe,
                        &pipeBytes
                        ));

                    /* Do not skip pipe switching sequence. */
                    offset = 0;
                }

                /* Compute the entry. */
#if gcdNONPAGED_MEMORY_CACHEABLE
                entryPhysical = (gctUINT8_PTR) commandBufferPhysical + offset;
#endif
                entryLogical  =                commandBufferLogical  + offset;
                entryLogicalBase  =            commandBufferObject->logical;
                entryMdlHandle    =            commandBufferHandle ;
                entryBytes        =            commandBufferSize     - offset;
            }
        }
    }

#if gcdDUMP_COMMAND
    bufferDumpLogical = commandBufferLogical + offset;
    bufferDumpBytes   = commandBufferSize    - offset;
#endif

#if gcdSECURE_USER
    /* Process user hints. */
    gcmkONERROR(_ProcessHints(Command, ProcessID, commandBufferObject));
#endif

    /* Determine the location to jump to for the command buffer being
    ** scheduled. */
    if (Command->newQueue)
    {
        /* New command queue, jump to the beginning of it. */
#if gcdNONPAGED_MEMORY_CACHEABLE
        exitPhysical = Command->physical;
#endif

#if gcdFLUSH_FIX
        exitLogical  = waitLinkLogical;
        exitBytes    = waitLinkBytes;
#else
        exitLogical  = Command->logical;
        exitBytes    = Command->offset + waitLinkBytes;
#endif
        exitLogicalBase = Command->logical;
        exitMdlHandle   = Command->queues[Command->index].physical;
    }
    else
    {
        /* Still within the preexisting command queue, jump to the new
           WAIT/LINK command sequence. */
#if gcdNONPAGED_MEMORY_CACHEABLE
        exitPhysical = waitLinkPhysical;
#endif
        exitLogical  = waitLinkLogical;
        exitLogicalBase = waitLinkLogicalBase;
        exitMdlHandle   = waitLinkMdlHandle;
        exitBytes       = waitLinkBytes;
    }

    /* Add a new WAIT/LINK command sequence. When the command buffer which is
       currently being scheduled is fully executed by the GPU, the FE will
       jump to this WAIT/LINK sequence. */
    gcmkONERROR(gckHARDWARE_WaitLink(
        hardware,
        waitLinkLogical,
        waitLinkLogicalBase,
        offset,
        &waitLinkBytes,
        &waitOffset,
        &waitSize,
        waitLinkMdlHandle
        ));

    /* Compute the location if WAIT command. */
    waitPhysical = (gctUINT8_PTR) waitLinkPhysical + waitOffset;
    waitLogical  = (gctUINT8_PTR) waitLinkLogical  + waitOffset;

#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the command queue cache. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        Command->kernelProcessID,
        gcvNULL,
        exitPhysical,
        exitLogical,
        exitBytes
        ));
#endif

    /* Determine the location of the LINK command in the command buffer. */
    commandBufferLink
        = (gctUINT8_PTR) commandBufferObject->logical
        +                commandBufferObject->offset;

    /* Generate a LINK from the end of the command buffer being scheduled
       back to the kernel command queue. */
    gcmkONERROR(gckHARDWARE_Link(
        hardware,
        commandBufferLink,
        exitLogicalBase,
        exitLogical,
        exitBytes,
        &linkBytes,
        exitMdlHandle
        ));
#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the command buffer cache. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        ProcessID,
        gcvNULL,
        commandBufferPhysical,
        commandBufferLogical,
        commandBufferSize
        ));
#endif

#if gcdFLUSH_FIX
    if (oldValue)
    {
        gctUINT32 targetPhysical;

        /* Fill MMU configuration command seqeunce. */
        gcmkONERROR(gckHARDWARE_ConfigMMU(
            hardware,
            mmuConfigureLogical,
            Command->kernel->mmu->mtlbLogical,
            offset,
            gcvNULL,
            gcvNULL,
            gcvNULL
            ));

        if(entryLogical >= entryLogicalBase)
        {
            gcmkONERROR(
                gckHARDWARE_ConvertLogicalMdl(hardware, entryLogicalBase, &targetPhysical, entryMdlHandle));
            targetPhysical= targetPhysical + ((gctUINT32)entryLogical - (gctUINT32)entryLogicalBase);
        }
        else
        {
            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        gckENTRYQUEUE_Enqueue(Command->kernel->eventObj,
                             &Command->queue,
                             targetPhysical,
                             entryBytes);

        gckOS_AtomGet(Command->os,
                      Command->kernel->hardware->IsrClkoff,
                      &oldValue
                      );
        if(!oldValue)
        {
            printk("ERR POWER OFF ~~ but COMMANDCommit BIG ERR \n");
        }

        gcmkONERROR(gckOS_AtomicExchange(Command->os,
                                         Command->kernel->hardware->pageTableDirty,
                                         0,
                                         &oldValue));

        /* Update entryLogical. */
        entryLogical = mmuConfigureLogical;
        entryLogicalBase = Command->logical;
        entryMdlHandle   = Command->queues[Command->index].physical;
        /* Update entryBytes. */
        entryBytes = mmuConfigureBytes;
    }
#endif

    /* Generate a LINK from the previous WAIT/LINK command sequence to the
       entry determined above (either the context or the command buffer).
       This LINK replaces the WAIT instruction from the previous WAIT/LINK
       pair, therefore we use WAIT metrics for generation of this LINK.
       This action will execHandleute the entire sequence. */
    gcmkONERROR(gckHARDWARE_Link(
        hardware,
        Command->waitLogical,
        entryLogicalBase,
        entryLogical,
        entryBytes,
        &Command->waitSize,
        entryMdlHandle
        ));
#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the cache for the link. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        Command->kernelProcessID,
        gcvNULL,
        Command->waitPhysical,
        Command->waitLogical,
        Command->waitSize
        ));
#endif

    gcmkDUMPCOMMAND(
        Command->os,
        Command->waitLogical,
        Command->waitSize,
        gceDUMP_BUFFER_LINK,
        gcvFALSE
        );

    gcmkDUMPCOMMAND(
        Command->os,
        contextDumpLogical,
        contextDumpBytes,
        gceDUMP_BUFFER_CONTEXT,
        gcvFALSE
        );

    gcmkDUMPCOMMAND(
        Command->os,
        bufferDumpLogical,
        bufferDumpBytes,
        gceDUMP_BUFFER_USER,
        gcvFALSE
        );

    gcmkDUMPCOMMAND(
        Command->os,
        waitLinkLogical,
        waitLinkBytes,
        gceDUMP_BUFFER_WAITLINK,
        gcvFALSE
        );

    /* Update the current pipe. */
    Command->pipeSelect = commandBufferObject->exitPipe;

    /* Update command queue offset. */
#if gcdFLUSH_FIX
    Command->offset  += reservedBytes;
#else
    Command->offset  += waitLinkBytes;
#endif
    Command->newQueue = gcvFALSE;

    /* Update address of last WAIT. */
    Command->waitPhysical = waitPhysical;
    Command->waitLogical  = waitLogical;
    Command->waitSize     = waitSize;

    /* Update queue tail pointer. */
    gcmkONERROR(gckHARDWARE_UpdateQueueTail(
        hardware, Command->logical, Command->offset
        ));

#if gcdDUMP_COMMAND
    gcmkPRINT("@[kernel.commit]");
#endif
#endif /* gcdNULL_DRIVER */

    /* Release the context switching mutex. */
    gcmkONERROR(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
    contextAcquired = gcvFALSE;

    /* Release the command queue. */
    gcmkONERROR(gckCOMMAND_ExitCommit(Command, gcvFALSE));
    commitEntered = gcvFALSE;

    /* Loop while there are records in the queue. */
    while (EventQueue != gcvNULL)
    {
        if (needCopy)
        {
            /* Point to stack record. */
            eventRecord = &_eventRecord;

            /* Copy the data from the client. */
            gcmkONERROR(gckOS_CopyFromUserData(
                Command->os, eventRecord, EventQueue, gcmSIZEOF(gcsQUEUE)
                ));
        }
        else
        {
            /* Map record into kernel memory. */
            gcmkONERROR(gckOS_MapUserPointer(Command->os,
                                             EventQueue,
                                             gcmSIZEOF(gcsQUEUE),
                                             &pointer));

            eventRecord = pointer;
        }

        /* Append event record to event queue. */
        gcmkONERROR(gckEVENT_AddList(
            Command->kernel->eventObj, &eventRecord->iface, gcvKERNEL_PIXEL, gcvTRUE, gcvFALSE
            ));

        /* Next record in the queue. */
        nextEventRecord = eventRecord->next;

        if (!needCopy)
        {
            /* Unmap record from kernel memory. */
            gcmkONERROR(gckOS_UnmapUserPointer(
                Command->os, EventQueue, gcmSIZEOF(gcsQUEUE), (gctPOINTER *) eventRecord
                ));

            eventRecord = gcvNULL;
        }

        EventQueue = nextEventRecord;
    }

#if gcdPOWER_MANAGEMENT
    if (Command->kernel->eventObj->queueHead == gcvNULL)
    {
        /* Commit done event by which work thread knows all jobs done. */
        gcmkVERIFY_OK(
            gckEVENT_CommitDone(Command->kernel->eventObj, gcvKERNEL_PIXEL));
    }
#endif

    /* Submit events. */
    status = gckEVENT_Submit(Command->kernel->eventObj, gcvTRUE, gcvFALSE);

    if (status == gcvSTATUS_INTERRUPTED)
    {
        gcmkTRACE(
            gcvLEVEL_INFO,
            "%s(%d): Intterupted in gckEVENT_Submit",
            __FUNCTION__, __LINE__
            );
        status = gcvSTATUS_OK;
    }
    else
    {
        gcmkONERROR(status);
    }

    /* Unmap the command buffer pointer. */
    if (commandBufferMapped)
    {
        gcmkONERROR(gckOS_UnmapUserPointer(
            Command->os,
            CommandBuffer,
            gcmSIZEOF(struct _gcoCMDBUF),
            commandBufferObject
            ));

        commandBufferMapped = gcvFALSE;
    }

    /* Return status. */
    gcmkFOOTER();
    return gcvSTATUS_OK;

OnError:
    if ((eventRecord != gcvNULL) && !needCopy)
    {
        /* Roll back. */
        gcmkVERIFY_OK(gckOS_UnmapUserPointer(
            Command->os,
            EventQueue,
            gcmSIZEOF(gcsQUEUE),
            (gctPOINTER *) eventRecord
            ));
    }

    if (contextAcquired)
    {
        /* Release the context switching mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
    }

    if (commitEntered)
    {
        /* Release the command queue mutex. */
        gcmkVERIFY_OK(gckCOMMAND_ExitCommit(Command, gcvFALSE));
    }

    /* Unmap the command buffer pointer. */
    if (commandBufferMapped)
    {
        gcmkVERIFY_OK(gckOS_UnmapUserPointer(
            Command->os,
            CommandBuffer,
            gcmSIZEOF(struct _gcoCMDBUF),
            commandBufferObject
            ));
    }

    /* Return status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Reserve
**
**  Reserve space in the command queue.  Also acquire the command queue mutex.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object.
**
**      gctSIZE_T RequestedBytes
**          Number of bytes previously reserved.
**
**  OUTPUT:
**
**      gctPOINTER * Buffer
**          Pointer to a variable that will receive the address of the reserved
**          space.
**
**      gctSIZE_T * BufferSize
**          Pointer to a variable that will receive the number of bytes
**          available in the command queue.
*/
gceSTATUS
gckCOMMAND_Reserve(
    IN gckCOMMAND Command,
    IN gctSIZE_T RequestedBytes,
    OUT gctPOINTER * Buffer,
    OUT gctSIZE_T * BufferSize
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctSIZE_T bytes;
    gctSIZE_T requiredBytes;
    gctUINT32 requestedAligned;

    gcmkHEADER_ARG("Command=0x%x RequestedBytes=%lu", Command, RequestedBytes);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Compute aligned number of reuested bytes. */
    requestedAligned = gcmALIGN(RequestedBytes, Command->alignment);

    /* Another WAIT/LINK command sequence will have to be appended after
       the requested area being reserved. Compute the number of bytes
       required for WAIT/LINK at the location after the reserved area. */
    gcmkONERROR(gckHARDWARE_WaitLink(
        Command->kernel->hardware,
        gcvNULL,
        gcvNULL,
        Command->offset + requestedAligned,
        &requiredBytes,
        gcvNULL,
        gcvNULL,
        gcvNULL
        ));

    /* Compute total number of bytes required. */
    requiredBytes += requestedAligned;

    /* Compute number of bytes available in command queue. */
    bytes = Command->pageSize - Command->offset;

    /* Is there enough space in the current command queue? */
    if (bytes < requiredBytes)
    {
        /* Create a new command queue. */
        gcmkONERROR(_NewQueue(Command));

        /* Recompute the number of bytes in the new kernel command queue. */
        bytes = Command->pageSize - Command->offset;

        /* Still not enough space? */
        if (bytes < requiredBytes)
        {
            /* Rare case, not enough room in command queue. */
            gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
        }
    }

    /* Return pointer to empty slot command queue. */
    *Buffer = (gctUINT8 *) Command->logical + Command->offset;

    /* Return number of bytes left in command queue. */
    *BufferSize = bytes;

    /* Success. */
    gcmkFOOTER_ARG("*Buffer=0x%x *BufferSize=%lu", *Buffer, *BufferSize);
    return gcvSTATUS_OK;

OnError:
    /* Return status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Execute
**
**  Execute a previously reserved command queue by appending a WAIT/LINK command
**  sequence after it and modifying the last WAIT into a LINK command.  The
**  command FIFO mutex will be released whether this function succeeds or not.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object.
**
**      gctSIZE_T RequestedBytes
**          Number of bytes previously reserved.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Execute(
    IN gckCOMMAND Command,
    IN gctSIZE_T RequestedBytes
    )
{
    gceSTATUS status = gcvSTATUS_OK;

    gctPHYS_ADDR waitLinkPhysical;
    gctUINT8_PTR waitLinkLogical;
    gctUINT8_PTR waitLinkLogicalBase;
    gctPHYS_ADDR waitLinkMdlHandle;

    gctUINT32 waitLinkOffset;
    gctSIZE_T waitLinkBytes;

    gctPHYS_ADDR waitPhysical;
    gctPOINTER waitLogical;

    gctUINT32 waitOffset;
    gctSIZE_T waitBytes;

#if gcdNONPAGED_MEMORY_CACHEABLE
    gctPHYS_ADDR execPhysical;
#endif
    gctPOINTER execLogical;
    gctPOINTER execLogicalBase;
    gctPHYS_ADDR execHandle;

    gctSIZE_T execBytes;

    gcmkHEADER_ARG("Command=0x%x RequestedBytes=%lu", Command, RequestedBytes);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Compute offset for WAIT/LINK. */
    waitLinkOffset = Command->offset + RequestedBytes;

    /* Compute number of bytes left in command queue. */
    waitLinkBytes = Command->pageSize - waitLinkOffset;

    /* Compute the location if WAIT/LINK command sequence. */
    waitLinkPhysical = (gctUINT8_PTR) Command->physical + waitLinkOffset;
    waitLinkLogical  = (gctUINT8_PTR) Command->logical  + waitLinkOffset;
    waitLinkLogicalBase  = (gctUINT8_PTR) Command->logical;
    waitLinkMdlHandle    = Command->queues[Command->index].physical;

    /* Append WAIT/LINK in command queue. */
    gcmkONERROR(gckHARDWARE_WaitLink(
        Command->kernel->hardware,
        waitLinkLogical,
        waitLinkLogicalBase,
        waitLinkOffset,
        &waitLinkBytes,
        &waitOffset,
        &waitBytes,
        waitLinkMdlHandle
        ));

    /* Compute the location if WAIT command. */
    waitPhysical = (gctUINT8_PTR) waitLinkPhysical + waitOffset;
    waitLogical  =                waitLinkLogical  + waitOffset;

    /* Determine the location to jump to for the command buffer being
    ** scheduled. */
    if (Command->newQueue)
    {
        /* New command queue, jump to the beginning of it. */
#if gcdNONPAGED_MEMORY_CACHEABLE
        execPhysical = Command->physical;
#endif
        execLogical  = Command->logical;
        execBytes    = waitLinkOffset + waitLinkBytes;
    }
    else
    {
        /* Still within the preexisting command queue, jump directly to the
           reserved area. */
#if gcdNONPAGED_MEMORY_CACHEABLE
        execPhysical = (gctUINT8 *) Command->physical + Command->offset;
#endif
        execLogical  = (gctUINT8 *) Command->logical  + Command->offset;
        execBytes    = RequestedBytes + waitLinkBytes;
    }
    execLogicalBase = Command->logical;
    execHandle = Command->queues[Command->index].physical;

#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the cache. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        Command->kernelProcessID,
        gcvNULL,
        execPhysical,
        execLogical,
        execBytes
        ));
#endif

    /* Convert the last WAIT into a LINK. */
    gcmkONERROR(gckHARDWARE_Link(
        Command->kernel->hardware,
        Command->waitLogical,
        execLogicalBase,
        execLogical,
        execBytes,
        &Command->waitSize,
        execHandle
        ));
#if gcdNONPAGED_MEMORY_CACHEABLE
    /* Flush the cache. */
    gcmkONERROR(gckOS_CacheClean(
        Command->os,
        Command->kernelProcessID,
        gcvNULL,
        Command->waitPhysical,
        Command->waitLogical,
        Command->waitSize
        ));
#endif

    gcmkDUMPCOMMAND(
        Command->os,
        Command->waitLogical,
        Command->waitSize,
        gceDUMP_BUFFER_LINK,
        gcvFALSE
        );

    gcmkDUMPCOMMAND(
        Command->os,
        execLogical,
        execBytes,
        gceDUMP_BUFFER_KERNEL,
        gcvFALSE
        );

    /* Update the pointer to the last WAIT. */
    Command->waitPhysical = waitPhysical;
    Command->waitLogical  = waitLogical;
    Command->waitSize     = waitBytes;

    /* Update the command queue. */
    Command->offset  += RequestedBytes + waitLinkBytes;
    Command->newQueue = gcvFALSE;

    /* Update queue tail pointer. */
    gcmkONERROR(gckHARDWARE_UpdateQueueTail(
        Command->kernel->hardware, Command->logical, Command->offset
        ));

#if gcdDUMP_COMMAND
    gcmkPRINT("@[kernel.execute]");
#endif

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Stall
**
**  The calling thread will be suspended until the command queue has been
**  completed.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to an gckCOMMAND object.
**
**      gctBOOL FromPower
**          Determines whether the call originates from inside the power
**          management or not.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Stall(
    IN gckCOMMAND Command,
    IN gctBOOL FromPower
    )
{
#if gcdNULL_DRIVER
    /* Do nothing with infinite hardware. */
    return gcvSTATUS_OK;
#else
    gckOS os;
    gckHARDWARE hardware;
    gckEVENT eventObject;
    gceSTATUS status = gcvSTATUS_OK;
    gctSIGNAL signal = gcvNULL;
    gctUINT timer = 0;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Extract the gckOS object pointer. */
    os = Command->os;
    gcmkVERIFY_OBJECT(os, gcvOBJ_OS);

    /* Extract the gckHARDWARE object pointer. */
    hardware = Command->kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    /* Extract the gckEVENT object pointer. */
    eventObject = Command->kernel->eventObj;
    gcmkVERIFY_OBJECT(eventObject, gcvOBJ_EVENT);

    /* Allocate the signal. */
    gcmkONERROR(gckOS_CreateSignal(os, gcvTRUE, &signal));

    /* Append the EVENT command to trigger the signal. */
    gcmkONERROR(gckEVENT_Signal(eventObject, signal, gcvKERNEL_PIXEL));

    /* Submit the event queue. */
    gcmkONERROR(gckEVENT_Submit(eventObject, gcvTRUE, FromPower));

#if gcdDUMP_COMMAND
    gcmkPRINT("@[kernel.stall]");
#endif

    if (status == gcvSTATUS_CHIP_NOT_READY)
    {
        /* Error. */
        goto OnError;
    }

    do
    {
        /* Wait for the signal. */
        status = gckOS_WaitSignal(os, signal, gcdGPU_ADVANCETIMER_STALL);

        if (status == gcvSTATUS_TIMEOUT)
        {
#if gcmIS_DEBUG(gcdDEBUG_CODE)
            gctUINT32 idle;

            /* Read idle register. */
            gcmkVERIFY_OK(gckHARDWARE_GetIdle(
                hardware, gcvFALSE, &idle
                ));

            gcmkTRACE(
                gcvLEVEL_ERROR,
                "%s(%d): idle=%08x",
                __FUNCTION__, __LINE__, idle
                );

            gcmkONERROR(gckOS_MemoryBarrier(os, gcvNULL));

#ifdef __QNXNTO__
            gctUINT32 reg_cmdbuf_fetch;
            gctUINT32 reg_intr;

            gcmkVERIFY_OK(gckOS_ReadRegisterEx(
                Command->kernel->hardware->os, Command->kernel->core, 0x0664, &reg_cmdbuf_fetch
                ));

            if (idle == 0x7FFFFFFE)
            {
                /*
                 * GPU is idle so there should not be pending interrupts.
                 * Just double check.
                 *
                 * Note that reading interrupt register clears it.
                 * That's why we don't read it in all cases.
                 */
                gcmkVERIFY_OK(gckOS_ReadRegisterEx(
                    Command->kernel->hardware->os, Command->kernel->core, 0x10, &reg_intr
                    ));

                slogf(
                    _SLOG_SETCODE(1, 0),
                    _SLOG_CRITICAL,
                    "GALcore: Stall timeout (idle = 0x%X, command buffer fetch = 0x%X, interrupt = 0x%X)",
                    idle, reg_cmdbuf_fetch, reg_intr
                    );
            }
            else
            {
                slogf(
                    _SLOG_SETCODE(1, 0),
                    _SLOG_CRITICAL,
                    "GALcore: Stall timeout (idle = 0x%X, command buffer fetch = 0x%X)",
                    idle, reg_cmdbuf_fetch
                    );
            }
#endif
#endif
            /* Advance timer. */
            timer += gcdGPU_ADVANCETIMER_STALL;
        }
        else if (status == gcvSTATUS_INTERRUPTED)
        {
            gcmkONERROR(gcvSTATUS_INTERRUPTED);
        }

    }
    while (gcmIS_ERROR(status)
#if gcdGPU_TIMEOUT
           && (timer < Command->kernel->timeOut)
#endif
           );

    /* Bail out on timeout. */
    if (gcmIS_ERROR(status))
    {
        gcmkTRACE_N(
            gcvLEVEL_ERROR,
            gcmSIZEOF(gceCORE) + gcmSIZEOF(gctCONST_STRING) + gcmSIZEOF(gctINT),
            "[%d] %s(%d): command stall is out of time\n",
            hardware->core, __FUNCTION__, __LINE__
            );

        gckOS_DumpCallStack(os);

        /* Broadcast the stuck GPU. */
        gcmkONERROR(gckOS_Broadcast(
            os, hardware, gcvBROADCAST_GPU_STUCK
            ));
    }

    /* Delete the signal. */
    gcmkVERIFY_OK(gckOS_DestroySignal(os, signal));

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (signal != gcvNULL)
    {
        /* Free the signal. */
        gcmkVERIFY_OK(gckOS_DestroySignal(os, signal));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
#endif
}

/*******************************************************************************
**
**  gckCOMMAND_Attach
**
**  Attach user process.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to a gckCOMMAND object.
**
**      gctUINT32 ProcessID
**          Current process ID.
**
**  OUTPUT:
**
**      gckCONTEXT * Context
**          Pointer to a variable that will receive a pointer to a new
**          gckCONTEXT object.
**
**      gctSIZE_T * StateCount
**          Pointer to a variable that will receive the number of states
**          in the context buffer.
*/
gceSTATUS
gckCOMMAND_Attach(
    IN gckCOMMAND Command,
    OUT gckCONTEXT * Context,
    OUT gctSIZE_T * StateCount,
    IN gctUINT32 ProcessID
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctBOOL acquired = gcvFALSE;

    gcmkHEADER_ARG("Command=0x%x", Command);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Acquire the context switching mutex. */
    gcmkONERROR(gckOS_AcquireMutex(
        Command->os, Command->mutexContext, gcvINFINITE
        ));
    acquired = gcvTRUE;

    /* Construct a gckCONTEXT object. */
    gcmkONERROR(gckCONTEXT_Construct(
        Command->os,
        Command->kernel->hardware,
        ProcessID,
        Context
        ));

    /* Return the number of states in the context. */
    * StateCount = (* Context)->stateCount;

    /* Release the context switching mutex. */
    gcmkONERROR(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
    acquired = gcvFALSE;

    /* Success. */
    gcmkFOOTER_ARG("*Context=0x%x", *Context);
    return gcvSTATUS_OK;

OnError:
    /* Release mutex. */
    if (acquired)
    {
        /* Release the context switching mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
        acquired = gcvFALSE;
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckCOMMAND_Detach
**
**  Detach user process.
**
**  INPUT:
**
**      gckCOMMAND Command
**          Pointer to a gckCOMMAND object.
**
**      gckCONTEXT Context
**          Pointer to a gckCONTEXT object to be destroyed.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckCOMMAND_Detach(
    IN gckCOMMAND Command,
    IN gckCONTEXT Context
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctBOOL acquired = gcvFALSE;

    gcmkHEADER_ARG("Command=0x%x Context=0x%x", Command, Context);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Command, gcvOBJ_COMMAND);

    /* Acquire the context switching mutex. */
    gcmkONERROR(gckOS_AcquireMutex(
        Command->os, Command->mutexContext, gcvINFINITE
        ));
    acquired = gcvTRUE;

    /* Current context stored in all kernel sharing container. Pointer used for 4x context swith judgement.
    if not clear it, it may give an error logic to next new thread first time context switch. */
    if(Command->currContext == Context)
    {
        Command->currContext = gcvNULL;
    }

    /* Construct a gckCONTEXT object. */
    gcmkONERROR(gckCONTEXT_Destroy(Context));

    if (Command->currContext == Context)
    {
        /* Detach from gckCOMMAND object if the destoryed context is current context. */
        Command->currContext = gcvNULL;
    }

    /* Release the context switching mutex. */
    gcmkONERROR(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
    acquired = gcvFALSE;

    /* Return the status. */
    gcmkFOOTER();
    return gcvSTATUS_OK;

OnError:
    /* Release mutex. */
    if (acquired)
    {
        /* Release the context switching mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Command->os, Command->mutexContext));
        acquired = gcvFALSE;
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}
