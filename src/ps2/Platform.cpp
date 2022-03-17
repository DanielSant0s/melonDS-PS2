/*
    Copyright 2018 Hydr8gon

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <malloc.h>
#include <stdio.h>
#include <kernel.h>

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#include "../Platform.h"
#include "../Config.h"

namespace Platform
{

void StopEmu()
{
}

void* Thread_Create(int (*func)(unsigned int, void*))
{
    ee_thread_t thread_param;
	
	thread_param.gp_reg = &_gp;
    thread_param.func = (void*)func;
    thread_param.stack = malloc(0x10000);
    thread_param.stack_size = 0x10000;
    thread_param.initial_priority = 0x40;
	int thread = CreateThread(&thread_param);

    StartThread(thread, NULL);
    return (void*)thread;
}

void Thread_Free(void* thread)
{
    
}

void Thread_Wait(void* thread)
{
    ee_thread_status_t info;
    while (info.status != THS_WAITSUSPEND){
        ReferThreadStatus((int)thread, &info);
    }
}

void* Semaphore_Create()
{
    ee_sema_t sema_params;
    sema_params.max_count = 1;
    sema_params.init_count = 0;
    s32 sema = CreateSema(&sema_params);
    return (void*)sema;
}

void Semaphore_Free(void* sema)
{
    DeleteSema((s32)sema);
}

void Semaphore_Reset(void* sema)
{
    ee_sema_t semInfo;
    s32 osResult;
    osResult = ReferSemaStatus((s32)sema, &semInfo);
    if(semInfo.count > 0){
        PollSema((s32)sema);
    }
}

void Semaphore_Wait(void* sema)
{
    WaitSema((s32)sema);
}

void Semaphore_Post(void* sema)
{
    SignalSema((s32)sema);
}

bool MP_Init()
{
    return false;
}

void MP_DeInit()
{
}

int MP_SendPacket(u8* data, int len)
{
    return 0;
}

int MP_RecvPacket(u8* data, bool block)
{
    return 0;
}

bool TryLoadPCap(void* lib)
{
    return true;
}

bool LAN_Init()
{
    return false;
}

void LAN_DeInit()
{
}

int LAN_SendPacket(u8* data, int len)
{
    return 0;
}

void LAN_RXCallback(u_char* blarg, const struct pcap_pkthdr* header, const u_char* data)
{
}

int LAN_RecvPacket(u8* data)
{
    return 0;
}

}
