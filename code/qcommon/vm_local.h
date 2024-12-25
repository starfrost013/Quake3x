/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#ifndef VM_LOCAL_H
#define VM_LOCAL_H

#include "q_shared.h"
#include "qcommon.h"

#define	MAX_OPSTACK_SIZE	512
#define	PROC_OPSTACK_SIZE	30

// don't change
// Hardcoded in q3asm an reserved at end of bss
#define	PROGRAM_STACK_SIZE	0x10000

// for some buggy mods
#define	PROGRAM_STACK_EXTRA	(32*1024)

// reserved space for effective LOCAL+LOAD* checks
// also to avoid runtime range checks for many small arguments/structs in systemcalls
#define	VM_DATA_GUARD_SIZE	1024

// guard size must cover at least function arguments area
#if VM_DATA_GUARD_SIZE < 256
#undef VM_DATA_GUARD_SIZE
#define VM_DATA_GUARD_SIZE 256
#endif

// flags for vm_rtChecks cvar
#define VM_RTCHECK_PSTACK  1
#define VM_RTCHECK_OPSTACK 2
#define VM_RTCHECK_JUMP    4
#define VM_RTCHECK_DATA    8

typedef struct {
	int32_t	value;     // 32
	byte	op;        // 8
	byte	opStack;   // 8
	unsigned jused:1;  // this instruction is a jump target
	unsigned swtch:1;  // indirect jump
	unsigned safe:1;   // non-masked OP_STORE*
	unsigned endp:1;   // for last OP_LEAVE instruction
	unsigned fpu:1;    // load into FPU register
	unsigned njump:1;  // near jump
} instruction_t;

//typedef void(*vmfunc_t)(void);

typedef union vmFunc_u {
	byte		*ptr;
	void (*func)(void);
} vmFunc_t;

struct vm_s {

	syscall_t	systemCall;
	byte		*dataBase;
	int32_t		*opStack;			// pointer to local function stack
	int32_t		*opStackTop;

	int32_t		programStack;		// the vm may be recursively entered
	int32_t		stackBottom;		// if programStack < stackBottom, error

	//------------------------------------

	const char	*name;				// module should be bare: "gameclient", not "gameclient.dll" or "vm/gameclient.qvm"
	vmIndex_t	index;

	// for dynamic linked modules
	void		*dllHandle;
	vmMainFunc_t entryPoint;
	dllSyscall_t dllSyscall;
	void (*destroy)(vm_t* self);

	intptr_t	*instructionPointers;

	int			callLevel;			// counts recursive VM_Call

	int32_t		*jumpTableTargets;
	int32_t		numJumpTableTargets;

	uint32_t	crc32sum;

	int			privateFlag;
};



#endif // VM_LOCAL_H
