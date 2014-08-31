/*
   Source for x86 emulator
   Copyright (c) 2003,2004,2005 Chris Eagle
   Copyright (c) 2004, Jeremy Cooper
      
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

#include <stdio.h>
#include <malloc.h>

#include "cpu.h"
#include "hooklist.h"
#include "emufuncs.h"
#include "seh.h"

#include "../idastruct/idastruct.h"

//masks to clear out bytes appropriate to the sizes above
dword SIZE_MASKS[] = {0, 0x000000FF, 0x0000FFFF, 0, 0xFFFFFFFF};

//masks to clear out bytes appropriate to the sizes above
dword SIGN_BITS[] = {0, 0x00000080, 0x00008000, 0, 0x80000000};

//masks to clear out bytes appropriate to the sizes above
#if defined(CYGWIN) || !defined(WIN32)
qword CARRY_BITS[] = {0, 0x00000100, 0x00010000, 0, 0x100000000ll};
#else
qword CARRY_BITS[] = {0, 0x00000100, 0x00010000, 0, 0x100000000};
#endif

byte BITS[] = {0, 8, 16, 0, 32};

const uchar parityValues[256] = {
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

//The cpu
dword debug_regs[8];
dword general[8];
dword initial_eip;
dword eip;
dword eflags;
dword control[5];
dword segBase[6];   //cached segment base addresses
word  segReg[6];
DescriptorTableReg gdtr;
DescriptorTableReg idtr;
static uquad tsc; //timestamp counter

static uint segmentBase;   //base address for next memory operation

dword seg3_map[] = {3, 0, 1, 2, 4, 5, 0, 0};

MemoryManager *mm = NULL; //The memory manager used for all memory access

static dword instStart;
//struct to describe an instruction being decoded
static AddrInfo source;
static AddrInfo dest;
static dword opsize;  //operand size for this instruction
static dword prefix;  //any prefix flags
static byte opcode;   //opcode, first or second byte (if first == 0x0F)

dword gpaSavePoint = 0xFFFFFFFF;
static bool makeImport = false;

IntrRecord *intrList = NULL;

int doEscape();

void setInterruptGate(dword base, dword interrupt_number, 
                      dword segment, dword handler) {
   segmentBase = dsBase;
   interrupt_number *= 8;
   writeMem(base + interrupt_number, handler, SIZE_WORD);
   writeMem(base + interrupt_number + 6, handler >> 16, SIZE_WORD);
   writeMem(base + interrupt_number + 2, segment, SIZE_WORD);
   writeMem(base + interrupt_number + 4, 0xEE00, SIZE_WORD);
}

void initIDTR() {
   idtr.base = mm->heap->calloc(0x200, 1);
   idtr.limit = 0x200;
   if (usingSEH()) {
      setInterruptGate(idtr.base, 0, cs, SEH_MAGIC);
      setInterruptGate(idtr.base, 1, cs, SEH_MAGIC);
      setInterruptGate(idtr.base, 3, cs, SEH_MAGIC);
   }
}

#ifdef __IDP__

int saveState(netnode &f) {
   unsigned char *buf = NULL;
   dword sz;
//   Buffer b(CPU_VERSION);
   Buffer b;

   //need to start writing version magic as first 4 bytes
   b.write((char*)debug_regs, sizeof(debug_regs));
   b.write((char*)general, sizeof(general));
   b.write((char*)&initial_eip, sizeof(initial_eip));
   b.write((char*)&eip, sizeof(eip));
   b.write((char*)&eflags, sizeof(eflags));
   b.write((char*)&control, sizeof(control));
   b.write((char*)segBase, sizeof(segBase));
   b.write((char*)segReg, sizeof(segReg));
   b.write((char*)&gdtr, sizeof(gdtr));
   b.write((char*)&idtr, sizeof(idtr));
   b.write((char*)&tsc, sizeof(tsc));
   b.write((char*)&gpaSavePoint, sizeof(gpaSavePoint));
   mm->save(b, esp);

/* VERSION(0)   
   saveHookList(b);
   saveModuleList(b);
*/
   //>= VERSION(1)
   saveHookList(b);
   saveModuleList(b);

   saveSEHState(b);
   
   if (!b.has_error()) {
   //
      // Delete any previous blob data in the IDA database node.
      //
      f.delblob(0, 'B');
   
      //
      // Convert the output blob object into a buffer and
      // store it in the database node.
      //
      sz = b.get_wlen();
   //   msg("writing blob of size %d.\n", sz);
      buf = b.get_buf();
/*
      for (int i = 0; i < sz; i += 16) {
         for (int j = 0; j < 16 && (j + i) < sz; j++) {
            msg("%2.2X ", buf[i + j]);
         }
         msg("\n");
      }
*/
      f.setblob(buf, sz, 0, 'B');
      return X86EMUSAVE_OK;
   }
   else {
      return X86EMUSAVE_FAILED;
   }
}

int loadState(netnode &f) {
   unsigned char *buf = NULL;
   dword sz;
   // Fetch the blob attached to the node.
   if ((buf = (unsigned char *)f.getblob(NULL, &sz, 0, 'B')) == NULL) return X86EMULOAD_NO_NETNODE;
//   msg("netnode found, sz = %d.\n", sz);
/*
   msg("netnode found, sz = %d.\n", sz);
   for (int i = 0; i < sz; i += 16) {
      for (int j = 0; j < 16 && (j + i) < sz; j++) {
         msg("%2.2X ", buf[i + j]);
      }
      msg("\n");
   }
*/
   Buffer b(buf, sz);
   //need to read version magic as first 4 bytes and skip stages depending on version
   b.read((char*)debug_regs, sizeof(debug_regs));
   b.read((char*)general, sizeof(general));
   b.read((char*)&initial_eip, sizeof(initial_eip));
   b.read((char*)&eip, sizeof(eip));
   b.read((char*)&eflags, sizeof(eflags));
   b.read((char*)&control, sizeof(control));
   b.read((char*)segBase, sizeof(segBase));
   b.read((char*)segReg, sizeof(segReg));
   b.read((char*)&gdtr, sizeof(gdtr));
   b.read((char*)&idtr, sizeof(idtr));
   b.read((char*)&tsc, sizeof(tsc));
   b.read((char*)&gpaSavePoint, sizeof(gpaSavePoint));
   mm = new MemoryManager(b);

   loadHookList(b);
   loadModuleList(b);

/*
   if (b.getVersion() == 0) {
      Buffer *r = getHookListBlob(b);
//      loadHookList(b);           //this needs to happen after modules have been loaded in new scheme
      loadModuleList(b);
      loadHookList(*r);
      delete r;
   }
   else {
      loadModuleList(b);
      loadHookList(b);
   }
*/

   loadSEHState(b);

   qfree(buf);
   
   if (!b.has_error() && idtr.base == 0) {
      initIDTR();
   }   

   return b.has_error() ? X86EMULOAD_CORRUPT : X86EMULOAD_OK;
}

#endif

void resetCpu() {
   memset(general, 0, sizeof(general));
   esp = mm ? mm->stack->getStackTop() : 0xC0000000;
   eip = 0xFFF0;
   eflags = 2;
   gdtr.limit = idtr.limit = 0xFFFF;
   cs = 0xF000;  //base = 0xFFFF0000, limit = 0xFFFF
   cr0 = 0x60000010;
   tsc = 0;
   //need to clear the heap in here as well then allocate a new idt
}

void initProgram(unsigned int entry, MemoryManager *mgr) {
   mm = mgr;
   esp = mm->stack->getStackTop();
   eip = entry;
   initIDTR();
}

//sign extension functions
//byte->word
word sebw(byte val) {
   short result = (char)val;
   return (word) result;
}

//word->dword
dword sewd(word val) {
   int result = (short)val;
   return (dword) result;
}

//byte->dword
dword sebd(byte val) {
   int result = (char)val;
   return (dword) result;
}

//dword->qword
qword sedq(dword val) {
   quad result = (int)val;
   return (qword) result;
}

//return a byte
byte readByte(dword addr) {
   return mm->readByte(addr);
}

//don't interface to IDA's get_word/long routines so
//that we can detect stack usage in readByte
word readWord(dword addr) {
   word result = readByte(addr + 1);
   result <<= 8;
   return result | readByte(addr);
}

dword readDword(dword addr) {
   dword result = readWord(addr + 2);
   result <<= 16;
   return result | readWord(addr);
}

//all reads from memory should be through this function
dword readMem(dword addr, byte size) {
   int result = 0;
   addr += segmentBase;
   switch (size) {
      case SIZE_BYTE:
         result = (int) readByte(addr);
         break;
      case SIZE_WORD:
         result = (int) readWord(addr);
         break;
      case SIZE_DWORD:
         result = (int) readDword(addr);
         break;
   }
   return result;
}

//store a byte
void writeByte(dword addr, byte val) {
   mm->writeByte(addr, val);
}

//don't interface to IDA's put_word/long routines so
//that we can detect stack usage in writeByte
void writeWord(dword addr, word val) {
   writeByte(addr, (byte)val);
   writeByte(addr + 1, (byte)(val >> 8));
}

void writeDword(dword addr, dword val) {
   if (makeImport) makeImportLabel(addr);
   writeWord(addr, (word)val);
   writeWord(addr + 2, (word)(val >> 16));
}

//all writes to memory should be through this function
void writeMem(dword addr, dword val, byte size) {
   addr += segmentBase;
   switch (size) {
      case SIZE_BYTE:
         writeByte(addr, (byte)val);
         break;
      case SIZE_WORD:
         writeWord(addr, (word)val);
         break;
      case SIZE_DWORD:
         writeDword(addr, val);
         break;
   }
}

void push(dword val, byte size) {
   segmentBase = ssBase;
   esp -= size;
   writeMem(esp, val, size);
}

dword pop(byte size) {
   segmentBase = ssBase;
   dword result = readMem(esp, size);
   esp += size;
   return result;
}

void doInterruptReturn() {
   if (intrList) {
      if (intrList->hasError) {
         pop(SIZE_DWORD);  //pop the saved error code
      }
      eip = pop(SIZE_DWORD);
      cs = pop(SIZE_DWORD);
      eflags = pop(SIZE_DWORD);
      IntrRecord *temp = intrList;
      intrList = intrList->next;
      free(temp);
   }  //else no interrupts to return from!
}

void initiateInterrupt(dword interrupt_number, dword saved_eip) {
   dword table = idtr.base + interrupt_number * 8;
   //need to pick segment reg value out of table as well
   dword handler = readMem(table, SIZE_WORD);
   handler |= (readMem(table + 6, SIZE_WORD) << 16);
   msg("Initiating INT %d processing w/ handler %x\n", interrupt_number, handler);
   push(eflags, SIZE_DWORD);
   push(cs, SIZE_DWORD);
   push(saved_eip, SIZE_DWORD);
   //need to push error code if required by interrupt_number
   //need to keep track of nested interrupts so that we know whether to 
   //pop off the error code during the associated iret
   eip = handler;
   IntrRecord *temp = (IntrRecord*) calloc(1, sizeof(IntrRecord));
   temp->next = intrList;
   intrList = temp;
   if (handler == SEH_MAGIC) {
      sehBegin(interrupt_number);
   }
}

//read according to specified n from eip location 
dword fetch(byte n) {
//   segmentBase = csBase;
   dword result = readMem(eip, n);
   eip += n;
   return result;
}

//fetch an unsigned quantity
dword fetchu(byte n) {
   return fetch(n) & SIZE_MASKS[n];
}

void fetchOperands16(AddrInfo *dest, AddrInfo *src) {
   byte modrm = fetchu(SIZE_BYTE);
   byte mod = MOD(modrm);
   byte rm = RM(modrm);
   dword disp = 0;
   if (mod != MOD_3) {
      switch (rm) {
         case 0:
            src->addr = ebx + esi;
            break;
         case 1:
            src->addr = ebp + esi;
            break;
         case 2:
            src->addr = ebx + edi;
            break;
         case 3:
            src->addr = ebp + edi;
            break;
         case 4:
            src->addr = esi;
            break;
         case 5:
            src->addr = edi;
            break;
         case 6:
            src->addr = ebp;
            break;
         case 7:
            src->addr = ebx;
            break;
      }
   }
   src->type = mod == MOD_3 ? TYPE_REG : TYPE_MEM;
   switch (mod) {
      case MOD_0:
         if (rm == 6) {
            src->addr = fetch(SIZE_WORD);
         }
         break;
      case MOD_1:
         disp = (char) fetch(SIZE_BYTE);
         break;
      case MOD_2:
         disp = (int) fetch(SIZE_WORD);
         break;
      case MOD_3:
         src->addr = rm;
         break;
   }
   if (src->type == TYPE_MEM) {
      src->addr += disp;
      src->addr &= SIZE_MASKS[SIZE_WORD];
   }
   dest->addr = REG(modrm);
   dest->type = TYPE_REG;
}

void fetchOperands(AddrInfo *dest, AddrInfo *src) {
   if (prefix & PREFIX_ADDR) {
      fetchOperands16(dest, src);
      return;
   }
   byte modrm = fetchu(SIZE_BYTE);
   byte mod = MOD(modrm);
   byte rm = RM(modrm);
   byte sib;
   dword disp = 0;
   byte hasSib = 0;
   if (mod != MOD_3) {
      if (rm == 4) {
         sib = fetchu(SIZE_BYTE);
         hasSib = 1;
      }
      else {
         src->addr = general[rm];
      }
   }
   src->type = mod == MOD_3 ? TYPE_REG : TYPE_MEM;
   switch (mod) {
      case MOD_0:
         if (rm == 5) {
            src->addr = fetch(SIZE_DWORD);
         }
         break;
      case MOD_1:
         disp = (char) fetch(SIZE_BYTE);
         break;
      case MOD_2:
         disp = (int) fetch(SIZE_DWORD);
         break;
      case MOD_3:
         src->addr = rm;
         break;
   }
   if (src->type == TYPE_MEM) {
      src->addr += disp;
      if (hasSib) {
         dword index = INDEX(sib);
         index = index == 4 ? 0 : general[index] * SCALE(sib);
         src->addr += index;
         dword base = BASE(sib);
         if (base == 5 && mod == MOD_0) {
            src->addr += fetch(SIZE_DWORD);
         }
         else {
            src->addr += general[base];
         }
      }
   }
   dest->addr = REG(modrm);
   dest->type = TYPE_REG;
}

void A_Ix() {
   dest.addr = 0;
   dest.type = TYPE_REG;
   source.addr = fetch(opsize);
   source.type = TYPE_IMM;
}

void decodeAddressingModes() {
   opsize = opcode & 1 ? opsize : SIZE_BYTE;
   switch (opcode & 0x7) {
      case 0: case 1:
         fetchOperands(&source, &dest);
         break;
      case 2: case 3:
         fetchOperands(&dest, &source);
         break;
      case 4: case 5:
         A_Ix();
         break;
   }
}

//set the segment for data storage and retrieval
// N/A for instruction fetches and stack push/pop
void setSegment() {
   if (prefix & SEG_MASK) {
      int i;
      int seg = PREFIX_CS;
      for (i = 0; i < 6; i++) {
         if (prefix & seg) {
            segmentBase = segBase[i];
            break;
         }
         seg <<= 1;
      }
   }
   else {  //? Not always the case
      segmentBase = dsBase;
   }
}

dword getOperand(AddrInfo *op) {
   dword mask = SIZE_MASKS[opsize];
   switch (op->type) {
      case TYPE_REG:
         if (opsize == SIZE_BYTE && op->addr >= 4) {
            //AH, CH, DH, BH
            return (general[op->addr - 4] >> 8) & mask;
         }
         return general[op->addr] & mask;
      case TYPE_IMM:
         return op->addr & mask;
      case TYPE_MEM:
         setSegment();
         return readMem(op->addr, opsize) & mask;
   }
   return 0;
}

void storeOperand(AddrInfo *op, dword val) {
   dword mask = SIZE_MASKS[opsize];
   val &= mask;
   if (op->type == TYPE_REG) {
      if (opsize == SIZE_BYTE && op->addr >= 4) {
         //AH, CH, DH, BH
         general[op->addr - 4] &= ~H_MASK;
         general[op->addr - 4] |= (val << 8); 
      }
      else {
         general[op->addr] &= ~SIZE_MASKS[opsize];
         general[op->addr] |= val; 
      }
   }
   else {
      setSegment();
      writeMem(op->addr, val, opsize);
   }
}

//deal with sign, zero, and parity flags
void setEflags(qword val, byte size) {
   val &= SIZE_MASKS[size]; //mask off upper bytes
   if (val) CLEAR(ZF);
   else SET(ZF);
   if (val & SIGN_BITS[size]) SET(SF);
   else CLEAR(SF);
   if (parityValues[val & 0xFF]) SET(PF);
   else CLEAR(PF);
}

void checkAddOverflow(dword op1, dword op2, dword sum) {
   dword mask = SIGN_BITS[opsize];
   if ((op1 & op2 & ~sum & mask) || (~op1 & ~op2 & sum & mask)) SET(OF);
   else CLEAR(OF);
}

dword add(qword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   qword result = (op1 & mask) + (op2 & mask);
   if (result & CARRY_BITS[opsize]) SET(CF);
   else CLEAR(CF);
   checkAddOverflow((dword)op1, op2, (dword)result);
   setEflags(result, opsize);
   return (dword) result & mask;
}

dword adc(qword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   qword result = (op1 & mask) + (op2 & mask) + C;
   if (result & CARRY_BITS[opsize]) SET(CF);
   else CLEAR(CF);
   checkAddOverflow((dword)op1, op2, (dword)result);
   setEflags(result, opsize);
   return (dword) result & mask;
}

void checkSubOverflow(dword op1, dword op2, dword diff) {
   dword mask = SIGN_BITS[opsize];
   if ((op1 & ~op2 & ~diff & mask) || (~op1 & op2 & diff & mask)) SET(OF);
   else CLEAR(OF);
}

dword sub(qword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   qword result = (op1 & mask) - (op2 & mask);
   if (result & CARRY_BITS[opsize]) SET(CF);
   else CLEAR(CF);
   checkSubOverflow((dword)op1, op2, (dword)result);
   setEflags(result, opsize);
   return (dword) result & mask;
}

dword sbb(qword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   qword result = (op1 & mask) - (op2 & mask) - C;
   if (result & CARRY_BITS[opsize]) SET(CF);
   else CLEAR(CF);
   checkSubOverflow((dword)op1, op2, (dword)result);
   setEflags(result, opsize);
   return (dword) result & mask;
}

dword AND(dword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   dword result = (op1 & mask) & (op2 & mask);
   CLEAR(CF | OF);
   setEflags(result, opsize);
   return result & mask;
}

dword OR(dword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   dword result = (op1 & mask) | (op2 & mask);
   CLEAR(CF | OF);
   setEflags(result, opsize);
   return result & mask;
}

dword XOR(dword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   dword result = (op1 & mask) ^ (op2 & mask);
   CLEAR(CF | OF);
   setEflags(result, opsize);
   return result & mask;
}

void cmp(qword op1, dword op2) {
   dword mask = SIZE_MASKS[opsize];
   qword result = (op1 & mask) - (op2 & mask);
   if (result & CARRY_BITS[opsize]) SET(CF);
   else CLEAR(CF);
   checkSubOverflow((dword)op1, op2, (dword)result);
   setEflags(result, opsize);
}

dword inc(qword op1) {
   dword oldCarry = C;
   op1 = add(op1, 1);
   CLEAR(CF);
   eflags |= oldCarry;
   return (dword) op1;
}

dword dec(qword op1) {
   dword oldCarry = C;
   op1 = sub(op1, 1);
   CLEAR(CF);
   eflags |= oldCarry;
   return (dword) op1;
}

void checkLeftOverflow(dword result, byte size) {
   dword msb = result & SIGN_BITS[size];
   if ((msb && C) || (!msb && NC)) CLEAR(OF);
   else SET(OF);
}

dword rol(qword op, byte amt) {
   if (amt) {
      op = op & SIZE_MASKS[opsize];
      op = (op >> (BITS[opsize] - amt)) | (op << amt);
      if (op & 1) SET(CF);
      else CLEAR(CF);
      if (amt == 1) {
         checkLeftOverflow((dword)op, opsize);
      }
   }
   return (dword) op & SIZE_MASKS[opsize];
}
                               
dword ror(qword op, byte amt) {
   if (amt) {
      op = op & SIZE_MASKS[opsize];
      op = (op << (BITS[opsize] - amt)) | (op >> amt);
      if (op & SIGN_BITS[opsize]) SET(CF);
      else CLEAR(CF);
      if (amt == 1) {
         dword shift = (dword)op << 1;
         shift = (shift ^ (dword)op) & SIGN_BITS[opsize];
         if (shift) SET(OF);
         else CLEAR(OF);
      }
   }
   return (dword) op & SIZE_MASKS[opsize];
}
                               
//probably could do this faster with bit shifts but I am not 
//that concerned with speed
dword rcl(qword op, byte amt) {
   if (amt) {
      if (C) op |= CARRY_BITS[opsize];  //setup current carry
      else op &= ~CARRY_BITS[opsize];
      for (int i = amt; i; i--) {
         qword temp = op & CARRY_BITS[opsize]; //get current carry
         op <<= 1;
         if (temp) op |= 1; //feed carry back in right and
      }
      if (op & CARRY_BITS[opsize]) SET(CF);  //set final carry
      else CLEAR(CF);
      if (amt == 1) {
         checkLeftOverflow((dword)op, opsize);
      }
   }
   return (dword) op & SIZE_MASKS[opsize];
}

//probably could do this faster with bit shifts but I am not 
//that concerned with speed
dword rcr(qword op, byte amt) {
   qword temp = C; //get initial carry
   if (amt) {
      for (int i = amt; i; i--) {
         if (temp) op |= CARRY_BITS[opsize];  //prepare to feed carry in from left
         else op &= ~CARRY_BITS[opsize];
         temp = op & 1; //get next carry
         op >>= 1;
      }
      if (temp) SET(CF);  //set final carry
      else CLEAR(CF);
      if (amt == 1) {
         dword shift = (dword)op << 1;
         shift = (shift ^ (dword)op) & SIGN_BITS[opsize];
         if (shift) SET(OF);
         else CLEAR(OF);
      }
   }
   return (dword) op & SIZE_MASKS[opsize];
}

dword shl(qword op, byte amt) {
   if (amt) {
      if (amt == 1) {
         checkLeftOverflow((dword)op, opsize);
      }
      op <<= amt;
      if (op & CARRY_BITS[opsize]) SET(CF);
      else CLEAR(CF);
   }
   setEflags(op, opsize);
   return (dword) op & SIZE_MASKS[opsize];
}

//mask op down to size before calling
dword shiftRight(qword op, byte amt) {
   if (amt) {
      dword final_carry = 1 << (amt - 1);
      if (op & final_carry) SET(CF);
      else CLEAR(CF);
      op >>= amt;
   }
   setEflags(op, opsize);
   return (dword) op;
}
                               
dword shr(qword op, byte amt) {
   if (amt == 1) {
      if (op & SIGN_BITS[opsize]) SET(OF);
      else CLEAR(OF);
   }
   return shiftRight(op & SIZE_MASKS[opsize], amt);
}
                               
dword sar(qword op, byte amt) {
   op = op & SIZE_MASKS[opsize];
   switch (opsize) {
      case SIZE_BYTE:
         op = sedq(sebd((byte)op));
         break;
      case SIZE_WORD:
         op = sedq(sewd((word)op));
         break;
      case SIZE_DWORD:
         op = sedq((dword)op);
         break;
   }
   if (amt == 1) {
      CLEAR(OF);
   }
   return shiftRight(op, amt);
}

dword shrd(qword op1, qword bits, byte amt) {
   if (amt) {
      dword newCarry = 1 << (amt - 1);
      if (op1 & newCarry) SET(CF);
      else CLEAR(CF);
      bits <<= (BITS[opsize] - amt);
      op1 = ((op1 & SIZE_MASKS[opsize]) >> amt) | bits;
   }
   setEflags(op1, opsize);
   return (dword) (op1 & SIZE_MASKS[opsize]);
}
                               
dword shld(qword op1, qword bits, byte amt) {
   if (amt) {
      dword newCarry = 1 << (BITS[opsize] - amt);
      if (op1 & newCarry) SET(CF);
      else CLEAR(CF);
      bits = (bits & SIZE_MASKS[opsize]) >> (BITS[opsize] - amt);
      op1 = (op1 << amt) | bits;
   }
   setEflags(op1, opsize);
   return (dword) (op1 & SIZE_MASKS[opsize]);
}

void dShift() {
   fetchOperands(&source, &dest);
   byte amt;
   if ((opcode & 7) == 4) {
      amt = fetch(SIZE_BYTE);
   }
   else {
      amt = ecx & 0xFF;
   }
   amt &= 0x1F;
   dword result;
   dword op1 = getOperand(&dest);
   dword op2 = getOperand(&source);
   if (opcode < 0xA8) {
      result = shld(op1, op2, amt);
   }
   else {
      result = shrd(op1, op2, amt);
   }
   storeOperand(&dest, result);
}

void doCall(dword addr) {
   hookfunc hook = findHook(addr);
//   hookfunc hook = findHook(instStart);
   if (hook) {
      (*hook)(mm, addr);
   }
   else if (isModuleAddress(addr)) {
      //this function is in a loaded module
      char *name = reverseLookupExport(addr);
      if (name) {
         (*checkForHook(name, addr, 0))(mm, addr);
      }
      else {
         msg("call to dll function that is not exported %X\n", addr);
      }
   }
   else {
      push(eip, SIZE_DWORD);
      eip = addr;
   }
}
                               
//handle instructions that begin w/ 0x0n
int doZero() {
   byte op = opcode & 0x0F;
   dword result;
   if ((op & 0x7) < 6) {
      decodeAddressingModes();
      dword op1 = getOperand(&dest);
      dword op2 = getOperand(&source);
      if (op < 8) { // ADD
         result = add(op1, op2);
      }
      else { // OR
         result = OR(op1, op2);
      }
      storeOperand(&dest, result);
   }
   else {
      switch (op) {
         case 0x06:
            push(es, SIZE_WORD);
            break;
         case 0x07:
            es = pop(SIZE_WORD);
            break;
         case 0x0E:
            push(cs, SIZE_WORD);
            break;
         case 0x0F:
            return doEscape();
      }
   }
   return 1;
}

//handle instructions that begin w/ 0x1n
int doOne() {
   byte op = opcode & 0x0F;
   dword result;
   if ((op & 0x7) < 6) {
      decodeAddressingModes();
      dword op1 = getOperand(&dest);
      dword op2 = getOperand(&source);
      if (op < 8) { // ADC
         result = adc(op1, op2);
      }
      else { // SBB
         result = sbb(op1, op2);
      }
      storeOperand(&dest, result);
   }
   else {
      switch (op) {
         case 6:
            push(ss, SIZE_WORD);
            break;
         case 7:
            ss = pop(SIZE_WORD);
            break;
         case 0xE:
            push(ds, SIZE_WORD);
            break;
         case 0xF:
            ds = pop(SIZE_WORD);
            break;
      }
   }
   return 1;
}

//handle instructions that begin w/ 0x2n
int doTwo() {
   byte op = opcode & 0x0F;
   dword result;
   if ((op & 0x7) < 6) {
      decodeAddressingModes();
      dword op1 = getOperand(&dest);
      dword op2 = getOperand(&source);
      if (op < 8) { // AND
         result = AND(op1, op2);
      }
      else { // SUB
         result = sub(op1, op2);
      }
      storeOperand(&dest, result);
   }
   else {
      switch (op) {
         case 0x6:
            prefix |= PREFIX_ES;
            return 0;
         case 7: { //DAA
            dword al = eax & 0xFF;
            if (((al & 0x0F) > 9) || (eflags & AF)) {
               dword old_al = al;
               SET(AF);
               al += 6;
               if (C || ((al ^ old_al) & 0x10)) SET(CF);
            }
            else {
               CLEAR(AF);
            }
            if (((al & 0xF0) > 0x90) || C) {
               al += 0x60;
               SET(CF);
            }
            else {
               CLEAR(CF);
            }
            eax = (eax & 0xFFFFFF00) | (al & 0xFF);
            setEflags(eax, SIZE_BYTE);
            break;
         }
         case 0xE:
            prefix |= PREFIX_CS;
            return 0;
         case 0xF: { //DAS
            dword al = eax & 0xFF;
            if (((al & 0x0F) > 9) || (eflags & AF)) {
               dword old_al = al;
               SET(AF);
               al -= 6;
               if (C || ((al ^ old_al) & 0x10)) SET(CF);
            }
            else {
               CLEAR(AF);
            }
            if (((al & 0xFF) > 0x9F) || C) {
               al -= 0x60;
               SET(CF);
            }
            else {
               CLEAR(CF);
            }
            eax = (eax & 0xFFFFFF00) | (al & 0xFF);
            setEflags(eax, SIZE_BYTE);
            break;
         }
      }
   }
   return 1;
}

//handle instructions that begin w/ 0x3n
int doThree() {
   byte op = opcode & 0x0F;
   if ((op & 0x7) < 6) {
      decodeAddressingModes();
      dword op1 = getOperand(&dest);
      dword op2 = getOperand(&source);
      if (op < 8) { // XOR
         storeOperand(&dest, XOR(op1, op2));
      }
      else { // CMP
         cmp(op1, op2);
      }
   }
   else {
      switch (op) {
         case 0x6:
            prefix |= PREFIX_SS;
            return 0;
         case 7: {//AAA
            dword al = eax & 0xFF;
            dword ax = eax & 0xFF00;
            if (((al & 0x0F) > 9) || (eflags & AF)) {
               SET(CF | AF);
               ax += 0x100;
               al += 6;
            }
            else {
               CLEAR(CF | AF);
            }
            ax |= al;
            eax = (eax & 0xFFFF0000) | (ax & 0xFF0F);
            break;
         }
         case 0xE:
            prefix |= PREFIX_DS;
            return 0;
         case 0xF: {//AAS
            dword al = eax & 0xFF;
            dword ax = eax & 0xFF00;
            if (((al & 0x0F) > 9) || (eflags & AF)) {
               SET(CF | AF);
               ax = (ax - 0x100) & 0xFF00;
               al -= 6;
            }
            else {
               CLEAR(CF | AF);
            }
            ax |= al;
            eax = (eax & 0xFFFF0000) | (ax & 0xFF0F);
            break;
         }
      }
   }
   return 1;
}

//handle instructions that begin w/ 0x4n
int doFour() {
   byte op = opcode & 0x0F;
   byte reg = op & 7;
   dword mask = SIZE_MASKS[opsize];
   //skip source setup, just read the register
   dword result = general[reg] & mask;
   dest.type = TYPE_REG;
   dest.addr = reg;
   if (op < 8) { // INC
      result = inc(result);
   }
   else { // DEC
      result = dec(result);
   }
   storeOperand(&dest, result);
   setEflags(result, opsize);
   return 1;
}

//handle instructions that begin w/ 0x5n
int doFive() {
   byte op = opcode & 0x0F;
   byte reg = op & 7;
   //skip source setup, just setup the destination
   dest.type = TYPE_REG;
   dest.addr = reg;
   if (op < 8) { // PUSH
      push(general[reg], opsize);
   }
   else { // POP
      storeOperand(&dest, pop(opsize));
   }
   return 1;
}

void stepd(byte size) {
   D ? (edi -= size) : (edi += size);
}

void steps(byte size) {
   D ? (esi -= size) : (esi += size);
}

void step(byte size) {
   stepd(size);
   steps(size);
}

//handle instructions that begin w/ 0x6n
int doSix() {
   byte op = opcode & 0x0F;
   dword result;
   int op1, op2;
   dword rep = prefix & PREFIX_REP;
   //skip source setup, just setup the destination
   dest.type = TYPE_REG;
   switch (op) {
      case 0: //PUSHA/PUSHAD
         result = esp;
         for (source.addr = EAX; source.addr <= EDI; source.addr++) {
            if (source.addr != ESP) push(general[source.addr], opsize);
            else push(result, opsize);
         }
         break;
      case 1: {//POPA/POPAD
         for (int j = EDI; j >= EAX; j--) { //need signed number for this test
            dest.addr = (dword)j;
            if (dest.addr == ESP) result = pop(opsize);
            else storeOperand(&dest, pop(opsize));
         }
         dest.addr = ESP;
         storeOperand(&dest, result);
         break;
              }
      case 2: //BOUND
         break;
      case 3: //ARPL
         break;
      case 0x4:
         prefix |= PREFIX_FS;
         return 0;
      case 0x5:
         prefix |= PREFIX_GS;
         return 0;
      case 0x6:
         prefix |= PREFIX_SIZE;
         opsize = SIZE_WORD;
         return 0;
      case 0x7:
         prefix |= PREFIX_ADDR;
         return 0;
      case 8: //PUSH Iv
         push(fetch(opsize), opsize);
         break;
      case 9: //IMUL Iv
         fetchOperands(&dest, &source);
         op1 = getOperand(&source);
         op2 = fetch(opsize);  //need to do some size alignment here
         result = op1 * op2;
         storeOperand(&dest, result);
         setEflags(result, opsize);
         break;
      case 0xA: //PUSH Ib
         //not certain this should be sign extended
         push(sebd(fetch(SIZE_BYTE)), opsize);
         break;
      case 0xB: //IMUL Ib
         fetchOperands(&dest, &source);
         op1 = getOperand(&source);
         op2 = fetch(SIZE_BYTE); //need to do some size alignement here
         result = op1 * op2;
         storeOperand(&dest, result);
         setEflags(result, SIZE_BYTE);
         break;
      case 0xC: //INS
         opsize = SIZE_BYTE;
      case 0xD: //INS
         segmentBase = esBase;
         if (rep) {
            while (ecx) {
//               writeMem(edi, eax, opsize);  //we are not really going to write data
               stepd(opsize);         
               ecx--;        //FAILS to take addr size into account
            }
         }
         else {
//            writeMem(edi, eax, opsize);  //we are not really going to write data
            stepd(opsize);         
         }
         break;
      case 0xE: //OUTS
         opsize = SIZE_BYTE;
      case 0xF: //OUTS
         source.type = TYPE_MEM;
         if (rep) {
            while (ecx) {
               //we will read the data but not do anything with it
               source.addr = esi;
               dword val = getOperand(&source);
               steps(opsize);
               ecx--;        //FAILS to take addr size into account
            }
         }
         else {
            //we will read the data but not do anything with it
            source.addr = esi;
            dword val = getOperand(&source);
            steps(opsize);
         }
         break;
   }
   return 1;
}

//handle instructions that begin w/ 0x7n
int doSeven() {
   byte op = opcode & 0x0F;
   dword imm = fetch(opsize);
   int branch = 0;
   switch (op) {
      case 0: //JO
         branch = O;
         break;
      case 1: //JNO
         branch = NO;
         break;
      case 2: //B/NAE/C
         branch = B;
         break;
      case 3: //NB/AE/NC
         branch = NB;
         break;
      case 4:  //E/Z
         branch = Z;
         break;
      case 5:  //NE/NZ
         branch = NZ;
         break;
      case 6:  //BE/NA
         branch = BE;
         break;
      case 7:  //NBE/A
         branch = A;
         break;
      case 8: //S
         branch = S;
         break;
      case 9: //NS
         branch = NS;
         break;
      case 0xA: //P/PE
         branch = P;
         break;
      case 0xB: //NP/PO
         branch = NP;
         break;
      case 0xC: //L/NGE
         branch = L;
         break;
      case 0xD: //NL/GE
         branch = GE;
         break;
      case 0xE: //LE/NG
         branch = LE;
         break;
      case 0xF: //NLE/G
         branch = G;
         break;
   }
   if (branch) {
      eip += (opsize == SIZE_BYTE) ? sebd(imm) : imm;
   }
   return 1;
}

//handle instructions that begin w/ 0x8n
int doEight() {
   byte op = opcode & 0x0F;
   dword op1, op2;
   byte size = op & 1 ? opsize : SIZE_BYTE;
   if (op < 4) {  //83 is sign extended byte->dword
                  //is 82 ever actually used?
      byte subop;
      opsize = size;
      fetchOperands(&source, &dest); //we will ignore Gx info
      subop = (byte) source.addr;
      op2 = fetch((op == 1) ? opsize : SIZE_BYTE);
      if (op == 3) op2 = sebd(op2);
      op1 = getOperand(&dest);
      //ADD, OR, ADC, SBB, AND, SUB, XOR, CMP
      switch (subop) {
         case 0: //ADD
            storeOperand(&dest, add(op1, op2));
            break;
         case 1: //OR
            storeOperand(&dest, OR(op1, op2));
            break;
         case 2: //ADC
            storeOperand(&dest, adc(op1, op2));
            break;
         case 3: //SBB
            storeOperand(&dest, sbb(op1, op2));
            break;
         case 4: //AND
            storeOperand(&dest, AND(op1, op2));
            break;
         case 5: //SUB
            storeOperand(&dest, sub(op1, op2));
            break;
         case 6: //XOR
            storeOperand(&dest, XOR(op1, op2));
            break;
         case 7: //CMP
            cmp(op1, op2);
            break;
      }
   }
   else if (op < 8) {
      opsize = size;
      fetchOperands(&source, &dest);
      if (op < 6) { //TEST
         AND(getOperand(&source), getOperand(&dest));
      }
      else { //XCHG
         dword temp = getOperand(&dest);
         storeOperand(&dest, getOperand(&source));
         dest.addr = source.addr;
         dest.type = source.type;
         storeOperand(&dest, temp);
      }
   }
   else if (op < 0xC) {  //MOVE
      opsize = size;
      decodeAddressingModes();
      storeOperand(&dest, getOperand(&source));
   }
   else {
      switch (op) {
         case 0xC: //MOVE reg seg - NOT using segment registers at the moment
            fetchOperands(&source, &dest); //generate the address
            storeOperand(&dest, segReg[seg3_map[source.addr]]); //store the address
            break;
         case 0xD: //LEA
            fetchOperands(&dest, &source); //generate the address
            storeOperand(&dest, source.addr); //store the address
            break;
         case 0xE: //MOVE seg reg - NOT using segment registers at the moment
            fetchOperands(&dest, &source); //generate the address
            segReg[seg3_map[source.addr]] = (word)general[source.addr];
            break;
         case 0xF: //POP
            fetchOperands(&source, &dest); //no source, just generate destination info
            storeOperand(&dest, pop(opsize));
            break;
      }
   }
   return 1;
}

//handle instructions that begin w/ 0x9n
int doNine() {
   byte op = opcode & 0x0F;
   dword temp;
   dest.type = TYPE_REG;
   if (op < 8) { //0 is actually NOP, but we do XCHG eax, eax here
      dest.addr = op & 7;
      temp = general[dest.addr];
      storeOperand(&dest, eax);
      dest.addr = 0;
      storeOperand(&dest, temp);
   }
   else {
      switch (op) {
         case 8: //CBW/CWDE
            dest.addr = EAX;
            if (opsize == SIZE_WORD) storeOperand(&dest, sebw(eax));
            else storeOperand(&dest, sewd(eax));
            break;
         case 9: //CWD/CDQ
            dest.addr = EDX;
            temp = eax & SIGN_BITS[opsize] ? 0xFFFFFFFF : 0;
            storeOperand(&dest, temp);
            break;
         case 0xA: //CALLF  //not required for my purposes?
            break;
         case 0xB: //FWAIT/WAIT  //not dealing with FP
            break;
         case 0xC: //PUSHF/PUSHFD
            push(eflags, opsize);
            break;
         case 0xD: //POPF/POPFD
            eflags = pop(opsize);
            break;
         case 0xE: //SAHF
            temp = eax >> 8;
            temp &= 0xD5;
            temp |= 2;
            eflags &= ~SIZE_MASKS[SIZE_BYTE];
            eflags |= temp;
            break;
         case 0xF: //LAHF
            temp = eflags & SIZE_MASKS[SIZE_BYTE] << 8;
            eax &= ~H_MASK;
            eax |= temp;
            break;
      }
   }
   return 1;
}

//handle instructions that begin w/ 0xAn
int doTen() {
   byte op = opcode & 0x0F;
   dword data;
   dword rep = prefix & PREFIX_REP;
   dword repne = prefix & PREFIX_REPNE;
   dword loop = prefix & (PREFIX_REP | PREFIX_REPNE);
   dest.addr = EAX;
   dest.type = TYPE_REG;
   switch (op) {
      case 0: // Segemented MOV moffs
         opsize = SIZE_BYTE;
         //break; // !! Should Not break. - NOTE error by daineng 20050704
      case 1: // Segemented MOV moffs
         source.addr = fetch(SIZE_DWORD);
         source.type = TYPE_MEM;
         storeOperand(&dest, getOperand(&source));
         break;
      case 2: // Segemented MOV moffs
         opsize = SIZE_BYTE;
         //break; // related to above error
      case 3: // Segemented MOV moffs
         dest.addr = fetch(SIZE_DWORD);
         dest.type = TYPE_MEM;
         storeOperand(&dest, eax);
         break;
      case 4:  //MOVS/MOVSB
         opsize = SIZE_BYTE;
      case 5:  //MOVS/MOVSW/MOVSD
         source.type = TYPE_MEM;
         if (rep) {
            while (ecx) {
               source.addr = esi;
               dword val = getOperand(&source);
               segmentBase = esBase;
               writeMem(edi, val, opsize);
               step(opsize);
               ecx--;        //FAILS to take addr size into account
            }
         }
         else {
            source.addr = esi;
            dword val = getOperand(&source);
            segmentBase = esBase;
            writeMem(edi, val, opsize);
            step(opsize);
         }
         break;
      case 6:  //CMPS/CMPSB
         opsize = SIZE_BYTE;
      case 7: //CMPS/CMPSW/CMPSD
         source.type = TYPE_MEM;
         if (loop) {
            while (ecx) {
               source.addr = esi;
               dword val = getOperand(&source);
               segmentBase = esBase;
               cmp(val, readMem(edi, opsize));
               step(opsize);
               ecx--;        //FAILS to take addr size into account
               if (rep && NZ) break;
               if (repne && Z) break;
            }
         }
         else {
            source.addr = esi;
            dword val = getOperand(&source);
            segmentBase = esBase;
            cmp(val, readMem(edi, opsize));
            step(opsize);
         }
         break;
      case 8: case 9: //TEST
         if (op == 8) {
            opsize = SIZE_BYTE;
         }
         data = fetch(opsize);
         AND(getOperand(&dest), data);
         break;
      case 0xA: //STOS/STOSB
         opsize = SIZE_BYTE;
      case 0xB: //STOS/STOSW/STOSD
         segmentBase = esBase;
         if (rep) {
            while (ecx) {
               writeMem(edi, eax, opsize);
               stepd(opsize);         
               ecx--;        //FAILS to take addr size into account
            }
         }
         else {
            writeMem(edi, eax, opsize);
            stepd(opsize);         
         }
         break;
      case 0xC: //LODS/LODSB
         opsize = SIZE_BYTE;
      case 0xD: //LODS/LODSW/LODSD
         source.type = TYPE_MEM;
         if (rep) {
            while (ecx) {
               source.addr = esi;
               dword val = getOperand(&source);
               eax &= ~SIZE_MASKS[opsize];
               eax |= val; 
               steps(opsize);
               ecx--;        //FAILS to take addr size into account
            }
         }
         else {
            source.addr = esi;
            dword val = getOperand(&source);
            eax &= ~SIZE_MASKS[opsize];
            eax |= val; 
            steps(opsize);
         }
         break;
      case 0xE: //SCAS/SCASB
         opsize = SIZE_BYTE;
      case 0xF: //SCAS/SCASW/SCASD
         segmentBase = esBase;
         if (loop) {
            while (ecx) {
               cmp(eax, readMem(edi, opsize));
               stepd(opsize);
               ecx--;        //FAILS to take addr size into account
               if (rep && NZ) break;
               if (repne && Z) break;
            }
         }
         else {
            cmp(eax, readMem(edi, opsize));
            stepd(opsize);
         }
         break;
   }
   return 1;
}

//handle instructions that begin w/ 0xBn
int doEleven() {
   byte op = opcode & 0x0F;
   dest.addr = op & 7;
   dest.type = TYPE_REG;
   if (op < 8) {
      dword data = fetch(SIZE_BYTE);
      if (op < 4) {
         opsize = SIZE_BYTE;
         storeOperand(&dest, data);
      }
      else {
         general[dest.addr & 3] &= ~H_MASK;
         data <<= 8;
         general[dest.addr & 3] |= (data & H_MASK);
      }
   }
   else {
      storeOperand(&dest, fetch(opsize));
   }
   return 1;
}

//handle instructions that begin w/ 0xCn
int doTwelve() {
   byte op = opcode & 0x0F;
   byte subop;
   dword delta, temp;
   switch (op) {
      case 0: //
         opsize = SIZE_BYTE;
      case 1: // SHFT Group 2
         fetchOperands(&source, &dest);
         subop = source.addr;
         delta = fetch(SIZE_BYTE) & 0x1F;  //shift amount
         if (delta) {
            temp = getOperand(&dest);
            switch (subop) {
               case 0: //ROL
                  storeOperand(&dest, rol(temp, delta));
                  break;
               case 1: //ROR
                  storeOperand(&dest, ror(temp, delta));
                  break;
               case 2: //RCL
                  storeOperand(&dest, rcl(temp, delta));
                  break;
               case 3: //RCR
                  storeOperand(&dest, rcr(temp, delta));
                  break;
               case 4:  //SHL/SAL
                  storeOperand(&dest, shl(temp, delta));
                  break;
               case 5:  //SHR
                  storeOperand(&dest, shr(temp, delta));
                  break;
               case 7: //SAR
                  storeOperand(&dest, sar(temp, delta));
                  break;
            }
         }
         break;
      case 2: //RETN Iw
         delta = fetchu(SIZE_WORD);
         eip = pop(SIZE_DWORD);
         esp += delta;
         break;
      case 3: //RETN
         eip = pop(SIZE_DWORD);
         if (eip == SEH_MAGIC) {
            sehReturn();
         }
         break;
      case 4:  //LES - NOT using segments now
         break;
      case 5:  //LDS - NOT using segments now
         break;
      case 6:  // MOV
         opsize = SIZE_BYTE;
      case 7: // MOV
         fetchOperands(&source, &dest);
         storeOperand(&dest, fetch(opsize));
         break;
      case 8: //ENTER
         delta = fetchu(SIZE_WORD);
         subop = fetchu(SIZE_BYTE);
         push(ebp, SIZE_DWORD);
         temp = esp;
         if (subop > 0) {
            while (--subop) {
               ebp -= 4;
               push(readMem(ebp, SIZE_DWORD), SIZE_DWORD);
            }
            push(temp, SIZE_DWORD);
         }
         ebp = temp;
         esp -= delta;
         break;
      case 9: //LEAVE
         esp = ebp;
         ebp = pop(SIZE_DWORD);
         break;
      case 0xA: //RETF Iw
         break;
      case 0xB: //RETF
         break;
      case 0xC: case 0xD: case 0xE: //INT 3 = 0xCC, INT Ib, INTO
         if (op == 0xD) subop = fetchu(SIZE_BYTE);  //this is the interrupt vector
         else subop = op == 0xC ? 3 : 4;  //3 == TRAP, 4 = O
         initiateInterrupt(subop, eip);
         break;
      case 0xF: //IRET
         doInterruptReturn();
         break;
   }
   return 1;
}

//handle instructions that begin w/ 0xDn
int doThirteen() {
   byte op = opcode & 0x0F;
   byte subop;
   dword delta, temp;
   switch (op) {
      case 0: case 2: //
         opsize = SIZE_BYTE;
      case 1: case 3: // SHFT Group 2
         fetchOperands(&source, &dest);
         subop = source.addr;
         delta = op < 2 ? 1 : ecx & 0x1F;  //shift amount
         temp = getOperand(&dest);
         switch (subop) {
            case 0: //ROL
               storeOperand(&dest, rol(temp, delta));
               break;
            case 1: //ROR
               storeOperand(&dest, ror(temp, delta));
               break;
            case 2: //RCL
               storeOperand(&dest, rcl(temp, delta));
               break;
            case 3: //RCR
               storeOperand(&dest, rcr(temp, delta));
               break;
            case 4:  //SHL/SAL
               storeOperand(&dest, shl(temp, delta));
               break;
            case 5:  //SHR
               storeOperand(&dest, shr(temp, delta));
               break;
            case 7: //SAR
               storeOperand(&dest, sar(temp, delta));
               break;
         }
         break;
      case 4: case 5: {//AAM / AAD
         dword base = fetchu(SIZE_BYTE);
         dword al = eax & 0xFF;
         dword ah = (eax >> 8) & 0xFF;
         dword ax = (op == 4) ? ((al / base) << 8) | (al % base) :
                                (al + ah * base) & 0xFF;
         setEflags(ax, SIZE_WORD);
         eax = (eax & ~SIZE_MASKS[SIZE_WORD]) | ax;
         break;
      }
      case 7: //XLAT/XLATB
         break;
/* Coprocessor escapes         
      case 8: //
         break;
      case 9: //
         break;
      case 0xA: //
         break;
      case 0xB: //
         break;
      case 0xC: //
         break;
      case 0xD: //
         break;
      case 0xE: //
         break;
      case 0xF: //
         break;
*/
   }
   return 1;
}

//handle instructions that begin w/ 0xEn
int doFourteen() {
   byte op = opcode & 0x0F;
   dword disp;
   dword cond;
   if (op < 4) {
      disp = fetch(SIZE_BYTE);
      if (op < 3) { //LOOPNE/LOOPNZ, LOOPE/LOOPZ, LOOP
         cond = op == 2 ? 1 : op == 0 ? NZ : Z;
         dest.addr = ECX;
         dest.type = TYPE_REG;
         storeOperand(&dest, getOperand(&dest) - 1);
         if (getOperand(&dest) && cond) {
            eip += sebd(disp);
         }
      }
      else {  //JCXZ
         if ((ecx & SIZE_MASKS[opsize]) == 0) {
            eip += sebd(disp);
         }
      }
   }
   switch (op) {
      case 4:  //IN
         fetchu(SIZE_BYTE);  //port number
         break;
      case 5:  //IN
         fetchu(SIZE_BYTE);  //port number
         break;
      case 6:  //OUT
         fetchu(SIZE_BYTE);  //port number
         break;
      case 7: //OUT
         fetchu(SIZE_BYTE);  //port number
         break;
      case 8: //CALL
         disp = fetch(opsize);
         if (opsize == SIZE_WORD) disp = sewd(disp);
         doCall(eip + disp);
         break;
      case 9: //JMP
         disp = fetch(opsize);
         if (opsize == SIZE_WORD) disp = sewd(disp);
         eip += disp;
         break;
      case 0xA: //JMP
         break;
      case 0xB: //JMP
         disp = sebd(fetch(SIZE_BYTE));
         eip += disp;
         break;
      case 0xC: //IN
         break;
      case 0xD: //IN
         break;
      case 0xE: //OUT
         break;
      case 0xF: //OUT
         break;
   }
   return 1;
}

//handle instructions that begin w/ 0xFn
int doFifteen() {
   byte op = opcode & 0x0F;
   qword temp, divisor;
   if ((op & 7) > 5) { //subgroup
      byte subop;
      fetchOperands(&source, &dest);
      subop = source.addr;
      if (op < 8) { //Unary group 3
         if (op == 6) opsize = SIZE_BYTE;
         switch (subop) {
            case 0: //TEST
               AND(getOperand(&dest), fetch(opsize));
               break;
            case 2: //NOT
               storeOperand(&dest, ~getOperand(&dest));
               break;
            case 3: //NEG
               temp = getOperand(&dest);
               storeOperand(&dest, sub(0, (dword)temp));
               if (temp) SET(CF);
               else CLEAR(CF);
               break;
            case 4: case 5: //MUL: IMUL: (CF/OF incorrect for IMUL
               source.addr = dest.addr;
               source.type = dest.type;
               temp = getOperand(&source);
               dest.addr = EAX;            //change dest to EAX
               dest.type = TYPE_REG;
               temp *= getOperand(&dest); //multiply by EAX
               if (opsize == SIZE_BYTE) {
                  opsize = SIZE_WORD;
                  storeOperand(&dest, (dword)temp);
                  temp >>= 8;
               }
               else {
                  storeOperand(&dest, (dword)temp);
                  dest.addr = EDX;
                  temp >>= opsize == SIZE_WORD ? 16 : 32;
                  storeOperand(&dest, (dword)temp);
               }
               if (temp) SET(CF | OF);
               else CLEAR(CF | OF);
               break;
            case 6: case 7: //DIV: IDIV: (does this work for IDIV?)
               source.addr = dest.addr;
               source.type = dest.type;
               if (opsize == SIZE_BYTE) temp = eax & 0xFFFF;
               else if (opsize == SIZE_WORD) {
                  temp = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF);
               }
               else {
                  temp = edx;
                  temp <<= 32;
                  temp |= eax;
               }
               divisor = getOperand(&source);
               if (divisor == 0) {
                  initiateInterrupt(0, initial_eip);
               }
               else {
                  dest.addr = EAX;
                  dest.type = TYPE_REG;
                  storeOperand(&dest, (dword) (temp / divisor));
                  dest.addr = EDX;
                  storeOperand(&dest, (dword) (temp % divisor));
               }
               break;
         }
      }
      else { //group4/5
         dword result;
         if (op == 0xE) opsize = SIZE_BYTE; //should only be a group 4
         if (subop < 2) { //INC/DEC
            if (subop == 0) result = inc(getOperand(&dest));
            else result = dec(getOperand(&dest));
            storeOperand(&dest, result);
         }
         else {
            switch (subop) {
               case 2: //CALLN
                  doCall(getOperand(&dest));
                  break;
               case 3: //CALLF
                  break;
               case 4: //JMPN
                  eip = getOperand(&dest);
                  break;
               case 5: //JMPF
                  break;
               case 6: //PUSH
                  push(getOperand(&dest), opsize);
                  break;
            }
         }
      }
   }
   else {
      switch (op) {
         case 0:
            prefix |= PREFIX_LOCK;
            return 0;
         case 1: //0xF1 icebp
            initiateInterrupt(1, initial_eip);
            break;
         case 2:
            prefix |= PREFIX_REPNE;
            return 0;
         case 3:
            prefix |= PREFIX_REP;
            return 0;
         case 4:  //HLT
            break;
         case 5:  //CMC
            eflags ^= CF;
            break;
         case 8: //CLC
            CLEAR(CF);
            break;
         case 9: //STC
            SET(CF);
            break;
         case 0xA: //CLI
            CLEAR(IF);
            break;
         case 0xB: //STI
            SET(IF);
            break;
         case 0xC: //CLD
            CLEAR(DF);
            break;
         case 0xD: //STD
            SET(DF);
            break;
      }
   }
   return 1;
}

int doSet(byte cc) {
   int set = 0;
   fetchOperands(&source, &dest);
   opsize = SIZE_BYTE;
   switch (cc) {
      case 0: //SO
         set = O;
         break;
      case 1: //SNO
         set = NO;
         break;
      case 2: //B/NAE/C
         set = B;
         break;
      case 3: //NB/AE/NC
         set = NB;
         break;
      case 4:  //E/Z
         set = Z;
         break;
      case 5:  //NE/NZ
         set = NZ;
         break;
      case 6:  //BE/NA
         set = BE;
         break;
      case 7:  //NBE/A
         set = A;
         break;
      case 8: //S
         set = S;
         break;
      case 9: //NS
         set = NS;
         break;
      case 0xA: //P/PE
         set = P;
         break;
      case 0xB: //NP/PO
         set = NP;
         break;
      case 0xC: //L/NGE
         set = L;
         break;
      case 0xD: //NL/GE
         set = GE;
         break;
      case 0xE: //LE/NG
         set = LE;
         break;
      case 0xF: //NLE/G
         set = G;
         break;
   }
   storeOperand(&dest, set ? 1 : 0);
   return 1;
} 

int doEscape() {
   dword result, regs;
   int op1, op2;
   opcode = fetchu(SIZE_BYTE);
   switch (opcode & 0xF0) {
      case 0x00: //LGDT, LIDT, SGDT, SIDT among others
         if (opcode < 0x02) {  //SGDT / SIDT
            DescriptorTableReg *dtr = opcode ? &idtr : &gdtr;
            decodeAddressingModes();
            opsize = SIZE_WORD;
            storeOperand(&dest, dtr->limit);
            opsize = SIZE_DWORD;
            dest.addr += 2;
            storeOperand(&dest, dtr->base);
         }
         break;
      case 0x20: //MOV to/from control/debug registers
         switch (opcode & 0xF) {
         case 0: //mov from control registers
            regs = fetchu(SIZE_BYTE);
            general[regs & 7] = control[(regs >> 3) & 7];
            break;
         case 1: //mov from debug registers
            regs = fetchu(SIZE_BYTE);
            general[regs & 7] = debug_regs[(regs >> 3) & 7];
            break;
         case 2:  //mov to control registers
            regs = fetchu(SIZE_BYTE);
            control[(regs >> 3) & 7] = general[regs & 7];
            break;
         case 3:  //mov to debug registers
            regs = fetchu(SIZE_BYTE);
            debug_regs[(regs >> 3) & 7] = general[regs & 7];
            break;
         }
         break;
      case 0x30: //
         if (opcode == 0x31) { //RDTSC
            edx = (dword) (tsc >> 32);
            eax = (dword) tsc;
         }
         break;
      case 0x80: //Jcc
         return doSeven(); //one byte Jcc handler
      case 0x90: //SET
         return doSet(opcode & 0xF);         
      case 0xA0: //IMUL, SHRD, SHLD
         if (((opcode & 7) == 4) || ((opcode & 7) == 5)) {
            dShift();
         }
         else if (opcode == 0xAF) { //IMUL
            fetchOperands(&dest, &source);
            op1 = getOperand(&source);
            op2 = getOperand(&dest);
            result = op1 * op2;
            storeOperand(&dest, result);
            setEflags(result, opsize);
         }
         else if (opcode == 0xA2) { //CPUID
            switch (eax) {
               case 0:
                  eax = 2;
                  ebx = 0x756E6547;  //"Genu"
                  ecx = 0x6C65746E;  //"ntel"
                  edx = 0x49656E69;  //"ineI"
                  break;
               case 1:
                  eax = 0xF10;
                  ebx = 0x0B;        //Xeon
                  ecx = 0;           //no features supported!
                  edx = 0;           //no features supported!
                  break;
               case 2:
               default:
                  break;
            }
         }
         break;
      case 0xB0: //MOVZX, MOVSX
         if ((opcode & 7) == 6) opsize = SIZE_BYTE;
         else opsize = SIZE_WORD;
         fetchOperands(&dest, &source);
         result = getOperand(&source);
         if (opcode & 8) { //MOVSX
            if (opsize == SIZE_BYTE) result = sebd((byte)result);
            else result = sewd((word)result);
         }
         opsize = SIZE_DWORD;
         storeOperand(&dest, result);
         break;
      case 0xC0:  //C8-CF BSWAP
         if (opcode >= 0xC8) {
            result = general[opcode & 0x7];
            general[opcode & 0x7] = (result << 24) | ((result << 8) & 0xFF0000) |
                                    ((result >> 24) & 0xFF) | ((result >> 8) & 0xFF00);
         }
         break;
   }
   return 1;
}

int executeInstruction() {
   int done = 0;
   int doTrap = eflags & TF;
   dest.addr = source.addr = prefix = 0;
   opsize = SIZE_DWORD;  //default
   segmentBase = csBase;
   instStart = csBase + eip;
   initial_eip = eip;
   //test breakpoint conditions here
   if (dr7 & 0x155) {  //minimal Dr enabled
      if (((dr7 & 1) && (eip == dr0)) ||
          ((dr7 & 4) && (eip == dr1)) ||
          ((dr7 & 0x10) && (eip == dr2)) ||
          ((dr7 & 0x40) && (eip == dr3))) {
          initiateInterrupt(1, initial_eip);
         //return from here with update eip as a result of jumping to exception handler
         //otherwise if we fall through first instruction in exception handler gets executed.
         return 0;
      }
   }

   if(strace)

	   struct_trace(eip);

   makeImport = eip == gpaSavePoint;
//msg("begin instruction, eip: 0x%x\n", eip);
   while (!done) {
      opcode = fetchu(SIZE_BYTE);
      if ((opcode & 0xF0) == 0x70) {
         opsize = SIZE_BYTE;
      }
      switch ((opcode >> 4) & 0x0F) {
         case 0x0:
            done = doZero();
            break;
         case 0x1:
            done = doOne();
            break;
         case 0x2:
            done = doTwo();
            break;
         case 0x3:
            done = doThree();
            break;
         case 0x4:
            done = doFour();
            break;
         case 0x5:
            done = doFive();
            break;
         case 0x6:
            done = doSix();
            break;
         case 0x7:
            done = doSeven();
            break;
         case 0x8:
            done = doEight();
            break;
         case 0x9:
            done = doNine();
            break;
         case 0xA:
            done = doTen();
            break;
         case 0xB:
            done = doEleven();
            break;
         case 0xC:
            done = doTwelve();
            break;
         case 0xD:
            done = doThirteen();
            break;
         case 0xE:
            done = doFourteen();
            break;
         case 0xF:
            done = doFifteen();
            break;
      }
   }
   tsc++;
   if (doTrap) {  //trace flag set
      eflags &= ~TF;   //clear TRAP flag
      initiateInterrupt(1, eip);
   }

//msg("end instruction, eip: 0x%x\n", eip);
   return 0;
}

/*
int main() {
   inst i;
   executeInstruction(&i);
   fprintf(stderr, "eax = %X\n", eax);
   return 0;
}
*/
