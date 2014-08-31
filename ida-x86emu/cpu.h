/*
   Headers for x86 emulator
   Copyright (c) 2003, 2004 Chris Eagle
   
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

#ifndef __CPU_H
#define __CPU_H

#include "x86defs.h"
#include "memmgr.h"

#define CPU_VERSION VERSION(1)

typedef struct _DescriptorTableReg_t {
   dword base;
   word limit;
} DescriptorTableReg;

extern dword debug_regs[8];
extern dword general[8];
extern dword initial_eip;
extern dword eip;
extern dword eflags;
extern dword control[5];
extern dword segBase[6];   //cached segment base addresses
extern word segReg[6];
extern DescriptorTableReg gdtr;
extern DescriptorTableReg idtr;

//masks to clear out bytes appropriate to the sizes above
extern dword SIZE_MASKS[5];

//masks to clear out bytes appropriate to the sizes above
extern dword SIGN_BITS[5];

//masks to clear out bytes appropriate to the sizes above
extern qword CARRY_BITS[5];

extern byte BITS[5];

extern dword gpaSavePoint;

extern MemoryManager *mm;

typedef struct _IntrRecord_t {
   bool hasError;
   struct _IntrRecord_t *next;
} IntrRecord;

typedef struct _AddrInfo_t {
   dword addr;
   byte type;
} AddrInfo;

//struct to describe an instruction being decoded
typedef struct _inst {
   AddrInfo source;
   AddrInfo dest;
   dword opsize;  //operand size for this instruction
   dword prefix;  //any prefix flags
   byte opcode;   //opcode, first or second byte (if first == 0x0F)
} inst;

// Status codes returned by the database blob reading routine
enum {
   X86EMULOAD_OK,                   // state loaded ok
   X86EMULOAD_VERSION_INCOMPATIBLE, // incompatible version
   X86EMULOAD_CORRUPT,              // corrupt/truncated
   X86EMULOAD_UNKNOWN_HOOKFN,       // contains hook to unknown hook function
   X86EMULOAD_NO_NETNODE,           // no save data present
   X86EMUSAVE_OK,                   // state save success
   X86EMUSAVE_FAILED                // state save failed (buffer problems)
};

void initProgram(unsigned int entry, MemoryManager *mgr);
void enableSEH();

void resetCpu();

void push(dword val, byte size);
dword pop(byte size);
dword readDword(dword addr);
void writeMem(dword addr, dword val, byte size);
dword readMem(dword addr, byte size);

int executeInstruction();
void doInterruptReturn();

#ifdef __IDP__

int saveState(netnode &f);
int loadState(netnode &f);

#endif

#endif

