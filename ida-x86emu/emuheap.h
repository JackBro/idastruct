/*
   Source for x86 emulator IdaPro plugin
   File: emuheap.h
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

#ifndef __EMUHEAP_H
#define __EMUHEAP_H

#include <stdio.h>
#include "buffer.h"

#define HEAP_ERROR 0xFFFFFFFF
#define HEAP_MAGIC 0xDEADBEEF

class MallocNode {
   friend class EmuHeap;
public:
   MallocNode(unsigned int size, unsigned int base);
   MallocNode(Buffer &b);

   ~MallocNode();

   bool contains(unsigned int addr);
   unsigned char readByte(unsigned int addr);
   void writeByte(unsigned int addr, unsigned char val);

   void save(Buffer &b);

private:
   unsigned int base;
   unsigned char *block;
   unsigned int size;
   MallocNode *next;
};

class EmuHeap {
   friend class MemoryManager;
public:
   EmuHeap(unsigned int baseAddr, unsigned int maxSize, EmuHeap *next = NULL);
   EmuHeap(Buffer &b);
   ~EmuHeap();
   unsigned int malloc(unsigned int size);
   unsigned int calloc(unsigned int nmemb, unsigned int size);
   unsigned int free(unsigned int addr);
   unsigned int realloc(unsigned int ptr, unsigned int size);

   unsigned char readByte(unsigned int addr);
   void writeByte(unsigned int addr, unsigned char val);
   EmuHeap *contains(unsigned int addr);
   
   unsigned int getHeapBase() {return base;};
   unsigned int getHeapSize() {return max - base;};
   EmuHeap *getNextHeap() {return nextHeap;};
   
   //careful to avoid memory leaks when calling this!
   void setNextHeap(EmuHeap *heap) {nextHeap = heap;};

   void save(Buffer &b);

private:
   EmuHeap(Buffer &b, unsigned int num_blocks);

   MallocNode *findNode(unsigned int addr);
   MallocNode *findMallocNode(unsigned int addr);
   unsigned int findBlock(unsigned int size);
   void insert(MallocNode *node);
   void readHeap(Buffer &b, unsigned int num_blocks);
   void writeHeap(Buffer &b);
   unsigned int base;
   unsigned int max;
   MallocNode *head;
   EmuHeap *nextHeap;
};

#endif
