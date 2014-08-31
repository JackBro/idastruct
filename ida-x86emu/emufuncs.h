/*
   Source for x86 emulator IdaPro plugin
   File: emufuncs.h
   Copyright (c) 2004, Chris Eagle
   
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option) 
   any later version.
   
   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for 
   more details.
   
   You should have received a copy of the GNU General Public License along with 
   this program; if not, write to the Free Software Foundation, Inc., 59 Temple 
   Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef __EMULATE_FUNCS_H
#define __EMULATE_FUNCS_H

#include <stdio.h>
#include "buffer.h"
#include "hooklist.h"

class MemoryManager;

void emu_HeapCreate(MemoryManager *mgr, unsigned int addr = 0);
void emu_HeapDestroy(MemoryManager *mgr, unsigned int addr = 0);
void emu_HeapAlloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_HeapFree(MemoryManager *mgr, unsigned int addr = 0);
void emu_GetProcessHeap(MemoryManager *mgr, unsigned int addr = 0);

void emu_VirtualAlloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_VirtualFree(MemoryManager *mgr, unsigned int addr = 0);
void emu_LocalAlloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_LocalFree(MemoryManager *mgr, unsigned int addr = 0);
void emu_GetProcAddress(MemoryManager *mgr, unsigned int addr = 0);
void emu_GetModuleHandle(MemoryManager *mgr, unsigned int addr = 0);
void emu_LoadLibrary(MemoryManager *mgr, unsigned int addr = 0);

void emu_malloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_calloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_realloc(MemoryManager *mgr, unsigned int addr = 0);
void emu_free(MemoryManager *mgr, unsigned int addr = 0);

void makeImportLabel(dword addr);
void saveModuleList(Buffer &b);
void loadModuleList(Buffer &b);

hookfunc checkForHook(char *funcName, dword funcAddr, dword moduleId);
void doImports(MemoryManager *mgr, dword import_drectory, dword image_base);
bool isModuleAddress(dword addr);
char *reverseLookupExport(dword addr);

typedef enum {NEVER, ASK, ALWAYS} emu_Actions;

extern int emu_alwaysLoadLibrary;
extern int emu_alwaysGetModuleHandle;

#endif
