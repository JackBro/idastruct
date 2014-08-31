/*
   Source for x86 emulator IdaPro plugin
   File: memmgr.cpp
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

#include <stdlib.h>

#ifdef __IDP__
#include <ida.hpp>
#include <idp.hpp>
#include <bytes.hpp>
#endif

#include "x86defs.h"
#include "seh.h"
#include "memmgr.h"
#include "emufuncs.h"

MemoryManager::MemoryManager(unsigned char *program, unsigned int minVaddr,
                             unsigned int maxVaddr) {
   initCommon(minVaddr, maxVaddr);
   this->program = program;
}

MemoryManager::MemoryManager(unsigned int minVaddr, unsigned int maxVaddr) {
   initCommon(minVaddr, maxVaddr);
   program = NULL;
}

MemoryManager::MemoryManager(Buffer &b) {
   b.read((char*)&minAddr, sizeof(minAddr));
   b.read((char*)&maxAddr, sizeof(maxAddr));
   stack = new EmuStack(b);
   heap = new EmuHeap(b);
}

void MemoryManager::save(Buffer &b, unsigned int sp) {
   b.write((char*)&minAddr, sizeof(minAddr));
   b.write((char*)&maxAddr, sizeof(maxAddr));
   stack->save(b, sp);
   heap->save(b);
}

MemoryManager::~MemoryManager() {
   delete stack;
   delete heap;
}

void MemoryManager::initStack(unsigned int stackTop, unsigned int maxSize) {
   if (stack) {
      stack->rebase(stackTop, maxSize);
   }
   else {
      stack = new EmuStack(stackTop, maxSize);
   }
}

void MemoryManager::initHeap(unsigned int heapBase, unsigned int maxSize) {
   heap = new EmuHeap(heapBase, maxSize);
}

unsigned int MemoryManager::addHeap(unsigned int maxSize) {
   EmuHeap *h, *p = NULL;
   for (h = heap; h; h = h->nextHeap) {
      p = h;
   }
   if (p) {
      //really need to check maxSize + max here against 0xFFFFFFFF
      p->nextHeap = new EmuHeap(p->max, maxSize);
   }
   return p ? p->base : 0;
}

unsigned int MemoryManager::destroyHeap(unsigned int handle) {
   EmuHeap *h, *p = NULL;
   for (h = heap; h; h = h->nextHeap) {
      if (h->base == handle) break;
      p = h;
   }
   if (p && h) {
      p->nextHeap = h->nextHeap;
      h->nextHeap = NULL;
      delete h;
      return 1;
   }
   return 0;
}

EmuHeap *MemoryManager::findHeap(unsigned int handle) {
   EmuHeap *h = NULL;
   for (h = heap; h; h = h->nextHeap) {
      if (h->base == handle) return h;
   }
   return NULL;
}

unsigned char MemoryManager::readByte(unsigned int addr) {
   EmuHeap *h;
   if (contains(addr)) {
#ifdef __IDP__
      //interface to IDA to read a byte
      //from virtual program space
      return get_byte(addr);
#else
      //assume user provided program space
      return program[addr - minAddr];
#endif
   }
   else if (stack && stack->contains(addr)) {
      return stack->readByte(addr);
   }
   else if (heap && (h = heap->contains(addr))) {
      return h->readByte(addr);
   }
   else if (isModuleAddress(addr)) {
      return *(unsigned char*)addr;
   }
   //else out of bounds memory access
//   memoryAccessException();
   return 0;
}

void MemoryManager::writeByte(unsigned int addr, unsigned char val) {
   EmuHeap *h;
   if (contains(addr)) {
#ifdef __IDP__
      //interface to IDA to write a byte
      //to virtual program space
   //   put_byte(addr, val);
      if (val == 0xFF) { //new version of ida (4.9) sees 0xFF as undefined?
         patch_byte(addr, 0);
      }
      patch_byte(addr, val);
#else
      //no IDA so assume user supplied program space
      program[addr - minAaddr] = val;
#endif
   }
   else if (stack && stack->contains(addr)) {
      stack->writeByte(addr, val);
#ifdef __IDP__
      updateStack(addr);
#endif
   }
   else if (heap && (h = heap->contains(addr))) {
      h->writeByte(addr, val);
   }
   //else out of bounds memory access
//   memoryAccessException();
}

bool MemoryManager::contains(unsigned int addr) {
   return (addr >= minAddr) && (addr < maxAddr);
}

void MemoryManager::initCommon(unsigned int minVaddr, unsigned int maxVaddr) {
   minAddr = minVaddr;
   maxAddr = maxVaddr;
   heap = NULL;
   stack = NULL;
}


