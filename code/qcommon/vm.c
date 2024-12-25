/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2012-2020 quake3e project

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
// vm.c -- virtual machine

/*


intermix code and data
symbol table

a dll has one imported function: VM_SystemCall
and one exported function: Perform


*/

#include "vm_local.h"

cvar_t	*vm_rtChecks;

#ifdef DEBUG
int		vm_debugLevel;
#endif

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

static struct vm_s vmTable[ VM_COUNT ];

static const char *vmName[ VM_COUNT ] = {
	"gameserver",
	"gameclient",
	"gameui"
};

static void VM_VmInfo_f( void );

#ifdef DEBUG
void VM_Debug( int level ) {
	vm_debugLevel = level;
}
#endif

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
#ifndef DEDICATED
	Cvar_Get( "vm_ui", "0", CVAR_ARCHIVE | CVAR_PROTECTED );	// !@# SHIP WITH SET TO 2
	Cvar_Get( "vm_gameclient", "0", CVAR_ARCHIVE | CVAR_PROTECTED );	// !@# SHIP WITH SET TO 2
#endif
	Cvar_Get( "vm_game", "0", CVAR_ARCHIVE | CVAR_PROTECTED );	// !@# SHIP WITH SET TO 2

	Cmd_AddCommand( "vminfo", VM_VmInfo_f );

	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation
=================
*/
vm_t *VM_Restart( vm_t *vm ) {

	syscall_t		systemCall;
	dllSyscall_t	dllSyscall;
	vmIndex_t		index;

	index = vm->index;
	systemCall = vm->systemCall;
	dllSyscall = vm->dllSyscall;

	VM_Free( vm );

	vm = VM_Create( index, systemCall, dllSyscall, VMI_NATIVE );
	return vm;
}


/*
=================
VM_LoadDll

Used to load a development dll instead of a virtual machine

TTimo: added some verbosity in debug
=================
*/
static void * QDECL VM_LoadDll( const char *name, vmMainFunc_t *entryPoint, dllSyscall_t systemcalls ) {

	const char	*gamedir = FS_GetCurrentGameDir();
	char		filename[ MAX_QPATH ];
	void		*libHandle;
	dllEntry_t	dllEntry;

	Com_sprintf( filename, sizeof( filename ), "%s%c%s" ARCH_STRING DLL_EXT, gamedir, PATH_SEP, name );

	libHandle = FS_LoadLibrary( filename );

	if ( !libHandle ) {
		Com_Printf( "VM_LoadDLL '%s' failed\n", filename );
		return NULL;
	}

	Com_Printf( "VM_LoadDLL '%s' ok\n", filename );

	dllEntry = /* ( dllEntry_t ) */ Sys_LoadFunction( libHandle, "dllEntry" );
	*entryPoint = /* ( dllSyscall_t ) */ Sys_LoadFunction( libHandle, "vmMain" );
	if ( !*entryPoint || !dllEntry ) {
		Sys_UnloadLibrary( libHandle );
		return NULL;
	}

	Com_Printf( "VM_LoadDll(%s) found **vmMain** at %p\n", name, *entryPoint );
	dllEntry( systemcalls );
	Com_Printf( "VM_LoadDll(%s) succeeded!\n", name );

	return libHandle;
}


/*
================
VM_Create

If image ends in .qvm it will be interpreted, otherwise
it will attempt to load as a system dll
================
*/
vm_t *VM_Create( vmIndex_t index, syscall_t systemCalls, dllSyscall_t dllSyscalls, vmInterpret_t interpret ) {
	int			remaining;
	const char	*name;
	vm_t		*vm;

	if ( !systemCalls ) {
		Com_Error( ERR_FATAL, "VM_Create: bad parms" );
	}

	if ( (unsigned)index >= VM_COUNT ) {
		Com_Error( ERR_DROP, "VM_Create: bad vm index %i", index );
	}

	remaining = Hunk_MemoryRemaining();

	vm = &vmTable[ index ];

	// see if we already have the VM
	if ( vm->name ) {
		if ( vm->index != index ) {
			Com_Error( ERR_DROP, "VM_Create: bad allocated vm index %i", vm->index );
			return NULL;
		}
		return vm;
	}

	name = vmName[ index ];

	vm->name = name;
	vm->index = index;
	vm->systemCall = systemCalls;
	vm->dllSyscall = dllSyscalls;
	vm->privateFlag = CVAR_PRIVATE;

	// try to load as a system dll
	Com_Printf( "Loading dll file %s.\n", name );
	vm->dllHandle = VM_LoadDll( name, &vm->entryPoint, dllSyscalls );

	// in the future, we will allow reading private cvars UNLESS we are the Squirrel scripting in the maps, so maps can't fuck with players 

	if ( vm->dllHandle ) {
		vm->privateFlag = 0; 
		return vm;
	}

	Com_Printf( "Failed to load DLL\n" );
	return NULL;

}


/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

	if( !vm ) {
		return;
	}

	if ( vm->callLevel ) {
		if ( !forced_unload ) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if ( vm->destroy )
		vm->destroy( vm );

	if ( vm->dllHandle )
		Sys_UnloadLibrary( vm->dllHandle );

	Com_Memset( vm, 0, sizeof( *vm ) );
}


void VM_Clear( void ) {
	int i;
	for ( i = 0; i < VM_COUNT; i++ ) {
		VM_Free( &vmTable[ i ] );
	}
}


void VM_Forced_Unload_Start(void) {
	forced_unload = 1;
}


void VM_Forced_Unload_Done(void) {
	forced_unload = 0;
}


/*
==============
VM_Call


Upon a system call, the stack will look like:

sp+32	parm1
sp+28	parm0
sp+24	return value
sp+20	return address
sp+16	local1
sp+14	local0
sp+12	arg1
sp+8	arg0
sp+4	return stack
sp		return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/

intptr_t QDECL VM_Call( vm_t *vm, int nargs, int callnum, ... )
{
	//vm_t	*oldVM;
	intptr_t r;
	int i;

	if ( !vm ) {
		Com_Error( ERR_FATAL, "VM_Call with NULL vm" );
	}

#ifdef DEBUG
	if ( vm_debugLevel ) {
	  Com_Printf( "VM_Call( %d )\n", callnum );
	}

	if ( nargs >= MAX_VMMAIN_CALL_ARGS ) {
		Com_Error( ERR_DROP, "VM_Call: nargs >= MAX_VMMAIN_CALL_ARGS" );
	}
#endif

	++vm->callLevel;
	// if we have a dll loaded, call it directly

		//rcg010207 -  see dissertation at top of VM_DllSyscall() in this file.
	int32_t args[MAX_VMMAIN_CALL_ARGS-1];
	va_list ap;
	va_start( ap, callnum );
	for ( i = 0; i < nargs; i++ ) {
		args[i] = va_arg( ap, int32_t );
	}
	va_end( ap );

	// add more arguments if you're changed MAX_VMMAIN_CALL_ARGS:

	r = vm->entryPoint( callnum, args[0], args[1], args[2] );

	--vm->callLevel;

	return r;
}

/*
==============
VM_NameToVM
==============
*/
static vm_t *VM_NameToVM( const char *name )
{
	vmIndex_t index;

	if ( !Q_stricmp( name, "gameserver" ) )
		index = VM_GAMESERVER;
	else if ( !Q_stricmp( name, "gameclient" ) )
		index = VM_GAMECLIENT;
	else if ( !Q_stricmp( name, "gameui" ) )
		index = VM_UI;
	else if ( !Q_stricmp( name, "map_squirrel" ) )
		index = VM_MAP_SQUIRREL;
	else {
		Com_Printf( " unknown VM name '%s'\n", name );
		return NULL;
	}

	if ( !vmTable[ index ].name ) {
		Com_Printf( " %s is not running.\n", name );
		return NULL;
	}

	return &vmTable[ index ];
}

/*
==============
VM_VmInfo_f
==============
*/
static void VM_VmInfo_f( void ) {
	const vm_t	*vm;
	int		i;

	Com_Printf( "Registered virtual machines:\n" );
	for ( i = 0 ; i < VM_COUNT ; i++ ) {
		vm = &vmTable[i];
		if ( !vm->name ) {
			continue;
		}
		Com_Printf( "%s : ", vm->name );

		// Q3X only has Native VM
		continue; 
	}
}

