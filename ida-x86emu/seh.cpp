
#include "cpu.h"
#include "seh.h"

static int seh_enable = 0;
static CONTEXT ctx;

struct CONTEXT *getContext() {
   return &ctx;
}

int usingSEH() {
   return seh_enable;
}

void saveSEHState(Buffer &b) {
   int dummy;
   b.write(&dummy, sizeof(dummy));
   b.write(&seh_enable, sizeof(seh_enable));
   b.write(&ctx, sizeof(ctx));
}

void loadSEHState(Buffer &b) {
   int dummy;
   b.read(&dummy, sizeof(dummy));
   b.read(&seh_enable, sizeof(seh_enable));
   b.read(&ctx, sizeof(ctx));
}

//Copy current CPU state into CONTEXT structure for Windows Exception Handling
//Note that the global ctx struct is the only place that Debug and Floating
//point registers are currently defined
void cpuToContext() {
   ctx.Dr0 = dr0;
   ctx.Dr1 = dr1;
   ctx.Dr2 = dr2;
   ctx.Dr3 = dr3;
   ctx.Dr6 = dr6;
   ctx.Dr7 = dr7;
   ctx.Eax = eax;
   ctx.Ebx = ebx;
   ctx.Ecx = ecx;
   ctx.Edx = edx;
   ctx.Edi = edi;
   ctx.Esi = esi;
   ctx.Ebp = ebp;
   ctx.Esp = esp;
//   ctx.Eip = eip;
   ctx.Eip = initial_eip;  //use address at which exception occurred
   ctx.EFlags = eflags;
   ctx.SegSs = ss;
   ctx.SegCs = cs;
   ctx.SegDs = ds;
   ctx.SegEs = es;
   ctx.SegFs = fs;
   ctx.SegGs = gs;
}

//Copy from CONTEXT structure into CPU state for Windows Exception Handling
//Note that the global ctx struct is the only place that Debug and Floating
//point registers are currently defined
void contextToCpu() {
   dr0 = ctx.Dr0;
   dr1 = ctx.Dr1;
   dr2 = ctx.Dr2;
   dr3 = ctx.Dr3;
   dr6 = ctx.Dr6;
   dr7 = ctx.Dr7;
   eax = ctx.Eax;
   ebx = ctx.Ebx;
   ecx = ctx.Ecx;
   edx = ctx.Edx;
   edi = ctx.Edi;
   esi = ctx.Esi;
   ebp = ctx.Ebp;
   esp = ctx.Esp;
   eip = ctx.Eip;
   eflags = ctx.EFlags;
   ss = ctx.SegSs;
   cs = ctx.SegCs;
   ds = ctx.SegDs;
   es = ctx.SegEs;
   fs = ctx.SegFs;
   gs = ctx.SegGs;
}

void initContext() {
   memset(&ctx, 0, sizeof(ctx));
}

void popContext() {
   byte *ptr = (byte*) &ctx;
   dword addr, i;
   dword ctx_size = (sizeof(CONTEXT) + 3) & ~3;  //round up to next dword
   addr = esp;
   for (i = 0; i < sizeof(CONTEXT); i++) {
      *ptr++ = (byte) readMem(addr++, SIZE_BYTE);
   }
   esp += ctx_size;
   contextToCpu();
}

dword pushContext() {
   byte *ptr = (byte*) &ctx;
   dword addr, i;
   dword ctx_size = (sizeof(CONTEXT) + 3) & ~3;  //round up to next dword
   cpuToContext();
   addr = esp -= ctx_size;
   for (i = 0; i < sizeof(CONTEXT); i++) {
      writeMem(addr++, *ptr++, SIZE_BYTE);
   }
   return esp;
}

void popExceptionRecord(EXCEPTION_RECORD *rec) {
   byte *ptr = (byte*) &rec;
   dword addr, i;
   dword rec_size = (sizeof(EXCEPTION_RECORD) + 3) & ~3;  //round up to next dword
   addr = esp;
   for (i = 0; i < sizeof(EXCEPTION_RECORD); i++) {
      *ptr++ = (byte) readMem(addr++, SIZE_BYTE);
   }
   esp += rec_size;
}

dword pushExceptionRecord(EXCEPTION_RECORD *rec) {
   byte *ptr = (byte*) rec;
   dword addr, i;
   dword rec_size = (sizeof(EXCEPTION_RECORD) + 3) & ~3;  //round up to next dword
   addr = esp -= rec_size;
   for (i = 0; i < sizeof(EXCEPTION_RECORD); i++) {
      writeMem(addr++, *ptr++, SIZE_BYTE);
   }
   return esp;
}

void doException(EXCEPTION_RECORD *rec) {
   dword err_ptr = readMem(fsBase, SIZE_DWORD);
   dword handler = readMem(err_ptr + 4, SIZE_DWORD);  //err->handler
   
   //do sanity checks on handler here?
   
   cpuToContext();
   dword ctx_ptr = pushContext();
   dword rec_ptr = pushExceptionRecord(rec);
   
   push(ctx_ptr, SIZE_DWORD);
   push(err_ptr, SIZE_DWORD);       //err_ptr == fsBase??
   push(rec_ptr, SIZE_DWORD);
   push(SEH_MAGIC, SIZE_DWORD);             //handler return address
//need to execute exception handler here setup flag to trap ret
//set eip to start of exception handler and resume fetching
   eip = handler;
}

void sehReturn() {
   EXCEPTION_RECORD rec;
   
   //need to check eax here to see if exception was handled
   //or if it needs to be kicked up to next SEH handler
   
   esp += 3 * SIZE_DWORD;  //clear off exception pointers
   
   popExceptionRecord(&rec);

   popContext();
   contextToCpu();
   //eip is now restored to pre exception location
   
   //need to fake an iret here
   doInterruptReturn();  //this clobbers EIP, CS, EFLAGS
   //so restore them here from ctx values
   eip = ctx.Eip;
   eflags = ctx.EFlags;
   cs = ctx.SegCs;
   msg("Performing SEH return\n");
}

void generateException(dword code) {
   if (seh_enable) {
      EXCEPTION_RECORD rec;
      rec.exceptionCode = code;
      rec.exceptionFlags = CONTINUABLE;   //nothing sophisticated here
      rec.exceptionRecord = 0;   //NULL
      rec.exceptionAddress = initial_eip;
      rec.numberParameters = 0;
      doException(&rec);
   }
}

void breakpointException() {
   generateException(BREAKPOINT_EXCEPTION);
}

void debugException() {
   generateException(DEBUG_EXCEPTION);
}

void divzeroException() {
   generateException(DIV_ZERO_EXCEPTION);
}

void memoryAccessException() {
   generateException(MEM_ACCESS);
}

void enableSEH() {
   initContext();
   seh_enable = 1;
}

void sehBegin(dword interrupt_number) {
   msg("Initiating SEH processing of INT %d\n", interrupt_number);
   switch (interrupt_number) {
   case 0:
      generateException(DIV_ZERO_EXCEPTION);
      break;   
   case 1:
      generateException(DEBUG_EXCEPTION);
      break;   
   case 3:
      generateException(BREAKPOINT_EXCEPTION);
      break;   
   }
}
