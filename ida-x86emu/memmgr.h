/*
   Source for x86 emulator IdaPro plugin
   File: memmgr.h
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

#ifndef __MEMORYMANAGER_H
#define __MEMORYMANAGER_H

#include <stdio.h>
#include "buffer.h"
#include "emustack.h"
#include "emuheap.h"

class MemoryManager {
public:
   MemoryManager(unsigned char *program, unsigned int minVaddr,
                 unsigned int maxVaddr);
   MemoryManager(unsigned int minVaddr, unsigned int maxVaddr);
   MemoryManager(Buffer &b);
   ~MemoryManager();

   bool contains(unsigned int addr);
   
   void initStack(unsigned int stackTop, unsigned int maxSize);
   void initHeap(unsigned int heapBase, unsigned int maxSize);
   
   //return "HANDLE" to new heap
   unsigned int addHeap(unsigned int maxSize);
   unsigned int destroyHeap(unsigned int handle);
   EmuHeap *findHeap(unsigned int handle);

   unsigned char readByte(unsigned int addr);
   void writeByte(unsigned int addr, unsigned char val);

   void save(Buffer &b, unsigned int sp);

   EmuStack *stack;
   EmuHeap *heap;

private:
   void initCommon(unsigned int minVaddr, unsigned int maxVaddr);

   unsigned char *program;
   unsigned int minAddr;
   unsigned int maxAddr;   
};


#endif
