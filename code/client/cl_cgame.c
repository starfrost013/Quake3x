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
// cl_gameclient.c  -- client system interaction with client game

#include "client.h"

#include "../botlib/botlib.h"

extern	botlib_export_t	*botlib_export;

//extern bool loadCamera(const char *name);
//extern void startCamera(int time);
//extern bool getCameraInfo(int time, vec3_t *origin, vec3_t *angles);

/*
====================
CL_GetGameState
====================
*/
static void CL_GetGameState( gameState_t *gs ) {
	*gs = cl.gameState;
}


/*
====================
CL_GetGlconfig
====================
*/
static void CL_GetGlconfig( glconfig_t *glconfig ) {
	*glconfig = cls.glconfig;
}


/*
====================
CL_GetUserCmd
====================
*/
static bool CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	// cmds[cmdNumber] is the last properly generated command

	// can't return anything that we haven't created yet
	if ( cl.cmdNumber - cmdNumber < 0 ) {
		Com_Error( ERR_DROP, "CL_GetUserCmd: cmdNumber (%i) > cl.cmdNumber (%i)", cmdNumber, cl.cmdNumber );
	}

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if ( cl.cmdNumber - cmdNumber >= CMD_BACKUP ) {
		return false;
	}

	*ucmd = cl.cmds[ cmdNumber & CMD_MASK ];

	return true;
}


/*
====================
CL_GetCurrentCmdNumber
====================
*/
static int CL_GetCurrentCmdNumber( void ) {
	return cl.cmdNumber;
}


/*
====================
CL_GetCurrentSnapshotNumber
====================
*/
static void CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = cl.snap.messageNum;
	*serverTime = cl.snap.serverTime;
}


/*
====================
CL_GetSnapshot
====================
*/
static bool CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;

	if ( cl.snap.messageNum - snapshotNumber < 0 ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber (%i) > cl.snapshot.messageNum (%i)", snapshotNumber, cl.snap.messageNum );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return false;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return false;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return false;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;
	for ( i = 0 ; i < count ; i++ ) {
		snapshot->entities[i] =
			cl.parseEntities[ ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1) ];
	}

	// FIXME: configstring changes and server commands!!!

	return true;
}


/*
=====================
CL_SetUserCmdValue
=====================
*/
static void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale ) {
	cl.gameclientUserCmdValue = userCmdValue;
	cl.gameclientSensitivity = sensitivityScale;
}


/*
=====================
CL_AddGameclientCommand
=====================
*/
static void CL_AddGameclientCommand( const char *cmdName ) {
	Cmd_AddCommand( cmdName, NULL );
}


/*
=====================
CL_ConfigstringModified
=====================
*/
static void CL_ConfigstringModified( void ) {
	const char	*old, *s;
	int			i, index;
	const char	*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( (unsigned) index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "%s: bad configstring index %i", __func__, index );
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// build the new gameState_t
	oldGs = cl.gameState;

	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = strlen( dup );

		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "%s: MAX_GAMESTATE_CHARS exceeded", __func__ );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	if ( index == CS_SYSTEMINFO ) {
		// parse serverId and other cvars
		CL_SystemInfoChanged( false );
	}
}


/*
===================
CL_GetServerCommand

Set up argc/argv for the given command
===================
*/
static bool CL_GetServerCommand( int serverCommandNumber ) {
	const char *s;
	const char *cmd;
	static char bigConfigString[BIG_INFO_STRING];
	int argc, index;

	// if we have irretrievably lost a reliable command, drop the connection
	if ( clc.serverCommandSequence - serverCommandNumber >= MAX_RELIABLE_COMMANDS ) {
		// when a demo record was started after the client got a whole bunch of
		// reliable commands then the client never got those first reliable commands
		if ( clc.demoplaying ) {
			Cmd_Clear();
			return false;
		}
		Com_Error( ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out" );
		return false;
	}

	if ( clc.serverCommandSequence - serverCommandNumber < 0 ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: requested a command not received" );
		return false;
	}

	index = serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 );
	s = clc.serverCommands[ index ];
	clc.lastExecutedServerCommand = serverCommandNumber;

	Com_DPrintf( "serverCommand: %i : %s\n", serverCommandNumber, s );

	if ( clc.serverCommandsIgnore[ index ] ) {
		Cmd_Clear();
		return false;
	}

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv(0);
	argc = Cmd_Argc();

	if ( !strcmp( cmd, "disconnect" ) ) {
		// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=552
		// allow server to indicate why they were disconnected
		if ( argc >= 2 )
			Com_Error( ERR_SERVERDISCONNECT, "Server disconnected - %s", Cmd_Argv( 1 ) );
		else
			Com_Error( ERR_SERVERDISCONNECT, "Server disconnected" );
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return false;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return false;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		return true;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		// clear notify lines and outgoing commands before passing
		// the restart to the gameclient
		Con_ClearNotify();
		// reparse the string, because Con_ClearNotify() may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		Com_Memset( cl.cmds, 0, sizeof( cl.cmds ) );
		cls.lastVidRestart = Sys_Milliseconds(); // hack for OSP mod
		return true;
	}

	// the clientLevelShot command is used during development
	// to generate 128*128 screenshots from the intermission
	// point of levels for the menu system to use
	// we pass it along to the gameclient to make appropriate adjustments,
	// but we also clear the console and notify lines here
	if ( !strcmp( cmd, "clientLevelShot" ) ) {
		// don't do it if we aren't running the server locally,
		// otherwise malicious remote servers could overwrite
		// the existing thumbnails
		if ( !com_sv_running->integer ) {
			return false;
		}
		// close the console
		Con_Close();
		// take a special screenshot next frame
		Cbuf_AddText( "wait ; wait ; wait ; wait ; screenshot levelshot\n" );
		return true;
	}

	// we may want to put a "connect to other server" command here

	// gameclient can now act on the command
	return true;
}


/*
====================
CL_CM_LoadMap

Just adds default parameters that gameclient doesn't need to know about
====================
*/
static void CL_CM_LoadMap( const char *mapname ) {
	int		checksum;

	CM_LoadMap( mapname, true, &checksum );
}


/*
====================
CL_ShutdonwGameclient

====================
*/
void CL_ShutdownGameClient( void ) {

	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_GAMECLIENT );
	cls.gameclientStarted = false;

	if ( !gameClientVm ) {
		return;
	}

	re.VertexLighting( false );

	VM_Call( gameClientVm, 0, CLIENT_SHUTDOWN );
	VM_Free( gameClientVm );
	gameClientVm = NULL;
	FS_VM_CloseFiles( H_GAMECLIENT );
}


static int FloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}


static void *VM_ArgPtr( intptr_t intValue ) {

	if ( !intValue || gameClientVm == NULL )
	  return NULL;

	return (void *)(intValue);
}


static bool CL_GetValue( char* value, int valueSize, const char* key ) {

	if ( !Q_stricmp( key, "trap_R_AddRefEntityToScene2" ) ) {
		Com_sprintf( value, valueSize, "%i", ENGINE_R_ADDREFENTITYTOSCENE2 );
		return true;
	}

	if ( !Q_stricmp( key, "trap_R_ForceFixedDLights" ) ) {
		Com_sprintf( value, valueSize, "%i", ENGINE_R_FORCEFIXEDDLIGHTS );
		return true;
	}

	if ( !Q_stricmp( key, "trap_R_AddLinearLightToScene_Q3E" ) && re.AddLinearLightToScene ) {
		Com_sprintf( value, valueSize, "%i", ENGINE_R_ADDLINEARLIGHTTOSCENE );
		return true;
	}

	if ( !Q_stricmp( key, "trap_IsRecordingDemo" ) ) {
		Com_sprintf( value, valueSize, "%i", ENGINE_IS_RECORDING_DEMO );
		return true;
	}

	return false;
}


static void CL_ForceFixedDlights( void ) {
	cvar_t *cv;

	cv = Cvar_Get( "r_dlightMode", "1", 0 );
	if ( cv ) {
		Cvar_CheckRange( cv, "1", "2", CV_INTEGER );
	}
}


/*
====================
CL_GameClientSystemCalls

The gameclient module is making a system call
====================
*/
static intptr_t CL_GameClientSystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case ENGINE_PRINT:
		Com_Printf( "%s", (const char*)VMA(1) );
		return 0;
	case ENGINE_ERROR:
		Com_Error( ERR_DROP, "%s", (const char*)VMA(1) );
		return 0;
	case ENGINE_MILLISECONDS:
		return Sys_Milliseconds();
	case ENGINE_CVAR_REGISTER:
		Cvar_Register( VMA(1), VMA(2), VMA(3), args[4], gameClientVm->privateFlag );
		return 0;
	case ENGINE_CVAR_UPDATE:
		Cvar_Update( VMA(1), gameClientVm->privateFlag );
		return 0;
	case ENGINE_CVAR_SET:
		Cvar_SetSafe( VMA(1), VMA(2) );
		return 0;
	case ENGINE_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBufferSafe( VMA(1), VMA(2), args[3], CVAR_PRIVATE );
		return 0;
	case ENGINE_ARGC:
		return Cmd_Argc();
	case ENGINE_ARGV:
		Cmd_ArgvBuffer( args[1], VMA(2), args[3] );
		return 0;
	case ENGINE_ARGS:
		Cmd_ArgsBuffer( VMA(1), args[2] );
		return 0;

	case ENGINE_FS_FOPENFILE:
		return FS_VM_OpenFile( VMA(1), VMA(2), args[3], H_GAMECLIENT );
	case ENGINE_FS_READ:
		FS_VM_ReadFile( VMA(1), args[2], args[3], H_GAMECLIENT );
		return 0;
	case ENGINE_FS_WRITE:
		FS_VM_WriteFile( VMA(1), args[2], args[3], H_GAMECLIENT );
		return 0;
	case ENGINE_FS_FCLOSEFILE:
		FS_VM_CloseFile( args[1], H_GAMECLIENT );
		return 0;
	case ENGINE_FS_SEEK:
		return FS_VM_SeekFile( args[1], args[2], args[3], H_GAMECLIENT );

	case ENGINE_SENDCONSOLECOMMAND: {
		const char *cmd = VMA(1);
		Cbuf_NestedAdd( cmd );
		return 0;
	}
	case ENGINE_ADDCOMMAND:
		CL_AddGameclientCommand( VMA(1) );
		return 0;
	case ENGINE_REMOVECOMMAND:
		Cmd_RemoveCommandSafe( VMA(1) );
		return 0;
	case ENGINE_SENDCLIENTCOMMAND:
		CL_AddReliableCommand( VMA(1), false );
		return 0;
	case ENGINE_UPDATESCREEN:
		// this is used during lengthy level loading, so pump message loop
		// Com_EventLoop();	// FIXME: if a server restarts here, BAD THINGS HAPPEN!
		// We can't call Com_EventLoop here, a restart will crash and this _does_ happen
		// if there is a map change while we are downloading at pk3.
		// ZOID
		SCR_UpdateScreen();
		return 0;
	case ENGINE_CM_LOADMAP:
		CL_CM_LoadMap( VMA(1) );
		return 0;
	case ENGINE_CM_NUMINLINEMODELS:
		return CM_NumInlineModels();
	case ENGINE_CM_INLINEMODEL:
		return CM_InlineModel( args[1] );
	case ENGINE_CM_TEMPBOXMODEL:
		return CM_TempBoxModel( VMA(1), VMA(2), /*int capsule*/ false );
	case ENGINE_CM_TEMPCAPSULEMODEL:
		return CM_TempBoxModel( VMA(1), VMA(2), /*int capsule*/ true );
	case ENGINE_CM_POINTCONTENTS:
		return CM_PointContents( VMA(1), args[2] );
	case ENGINE_CM_TRANSFORMEDPOINTCONTENTS:
		return CM_TransformedPointContents( VMA(1), args[2], VMA(3), VMA(4) );
	case ENGINE_CM_BOXTRACE:
		CM_BoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], /*int capsule*/ false );
		return 0;
	case ENGINE_CM_CAPSULETRACE:
		CM_BoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], /*int capsule*/ true );
		return 0;
	case ENGINE_CM_TRANSFORMEDBOXTRACE:
		CM_TransformedBoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9), /*int capsule*/ false );
		return 0;
	case ENGINE_CM_TRANSFORMEDCAPSULETRACE:
		CM_TransformedBoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9), /*int capsule*/ true );
		return 0;
	case ENGINE_CM_MARKFRAGMENTS:
		return re.MarkFragments( args[1], VMA(2), VMA(3), args[4], VMA(5), args[6], VMA(7) );
	case ENGINE_S_STARTSOUND:
		S_StartSound( VMA(1), args[2], args[3], args[4] );
		return 0;
	case ENGINE_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case ENGINE_S_CLEARLOOPINGSOUNDS:
		S_ClearLoopingSounds(args[1]);
		return 0;
	case ENGINE_S_ADDLOOPINGSOUND:
		S_AddLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case ENGINE_S_ADDREALLOOPINGSOUND:
		S_AddRealLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case ENGINE_S_STOPLOOPINGSOUND:
		S_StopLoopingSound( args[1] );
		return 0;
	case ENGINE_S_UPDATEENTITYPOSITION:
		S_UpdateEntityPosition( args[1], VMA(2) );
		return 0;
	case ENGINE_S_RESPATIALIZE:
		S_Respatialize( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case ENGINE_S_REGISTERSOUND:
		return S_RegisterSound( VMA(1), args[2] );
	case ENGINE_S_STARTBACKGROUNDTRACK:
		S_StartBackgroundTrack( VMA(1), VMA(2) );
		return 0;
	case ENGINE_R_LOADWORLDMAP:
		re.LoadWorld( VMA(1) );
		return 0;
	case ENGINE_R_REGISTERMODEL:
		return re.RegisterModel( VMA(1) );
	case ENGINE_R_REGISTERSKIN:
		return re.RegisterSkin( VMA(1) );
	case ENGINE_R_REGISTERSHADER:
		return re.RegisterShader( VMA(1) );
	case ENGINE_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip( VMA(1) );
	case ENGINE_R_REGISTERFONT:
		re.RegisterFont( VMA(1), args[2], VMA(3));
		return 0;
	case ENGINE_R_CLEARSCENE:
		re.ClearScene();
		return 0;
	case ENGINE_R_ADDREFENTITYTOSCENE:
		re.AddRefEntityToScene( VMA(1), false );
		return 0;
	case ENGINE_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), 1 );
		return 0;
	case ENGINE_R_ADDPOLYSTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), args[4] );
		return 0;
	case ENGINE_R_LIGHTFORPOINT:
		return re.LightForPoint( VMA(1), VMA(2), VMA(3), VMA(4) );
	case ENGINE_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case ENGINE_R_ADDADDITIVELIGHTTOSCENE:
		re.AddAdditiveLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case ENGINE_R_RENDERSCENE:
		re.RenderScene( VMA(1) );
		return 0;
	case ENGINE_R_SETCOLOR:
		re.SetColor( VMA(1) );
		return 0;
	case ENGINE_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
	case ENGINE_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA(2), VMA(3) );
		return 0;
	case ENGINE_R_LERPTAG:
		return re.LerpTag( VMA(1), args[2], args[3], args[4], VMF(5), VMA(6) );
	case ENGINE_GETGLCONFIG:
		CL_GetGlconfig( VMA(1) );
		return 0;
	case ENGINE_GETGAMESTATE:
		CL_GetGameState( VMA(1) );
		return 0;
	case ENGINE_GETCURRENTSNAPSHOTNUMBER:
		CL_GetCurrentSnapshotNumber( VMA(1), VMA(2) );
		return 0;
	case ENGINE_GETSNAPSHOT:
		return CL_GetSnapshot( args[1], VMA(2) );
	case ENGINE_GETSERVERCOMMAND:
		return CL_GetServerCommand( args[1] );
	case ENGINE_GETCURRENTCMDNUMBER:
		return CL_GetCurrentCmdNumber();
	case ENGINE_GETUSERCMD:
		return CL_GetUserCmd( args[1], VMA(2) );
	case ENGINE_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2) );
		return 0;
	case ENGINE_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();
	case ENGINE_KEY_ISDOWN:
		return Key_IsDown( args[1] );
	case ENGINE_KEY_GETCATCHER:
		return Key_GetCatcher();
	case ENGINE_KEY_SETCATCHER:
		// Don't allow the gameclient module to close the console
		Key_SetCatcher( args[1] | ( Key_GetCatcher( ) & KEYCATCH_CONSOLE ) );
		return 0;
	case ENGINE_KEY_GETKEY:
		return Key_GetKey( VMA(1) );

	case ENGINE_FLOOR:
		return FloatAsInt( floor( VMF(1) ) );
	case ENGINE_CEIL:
		return FloatAsInt( ceil( VMF(1) ) );
	case ENGINE_TESTPRINTINT:
		return sprintf( VMA(1), "%i", (int)args[2] );
	case ENGINE_TESTPRINTFLOAT:
		return sprintf( VMA(1), "%f", VMF(2) );
	case ENGINE_ACOS:
		return FloatAsInt( Q_acos( VMF(1) ) );

	case ENGINE_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMA(1) );
	case ENGINE_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMA(1) );
	case ENGINE_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case ENGINE_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMA(2) );
	case ENGINE_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMA(2), VMA(3) );

	case ENGINE_S_STOPBACKGROUNDTRACK:
		S_StopBackgroundTrack();
		return 0;

	case ENGINE_REAL_TIME:
		return Com_RealTime( VMA(1) );
	case ENGINE_SNAPVECTOR:
		Sys_SnapVector( VMA(1) );
		return 0;

	case ENGINE_CIN_PLAYCINEMATIC:
		return CIN_PlayCinematic(VMA(1), args[2], args[3], args[4], args[5], args[6]);

	case ENGINE_CIN_STOPCINEMATIC:
		return CIN_StopCinematic(args[1]);

	case ENGINE_CIN_RUNCINEMATIC:
		return CIN_RunCinematic(args[1]);

	case ENGINE_CIN_DRAWCINEMATIC:
		CIN_DrawCinematic(args[1]);
		return 0;

	case ENGINE_CIN_SETEXTENTS:
		CIN_SetExtents(args[1], args[2], args[3], args[4], args[5]);
		return 0;

	case ENGINE_R_REMAP_SHADER:
		re.RemapShader( VMA(1), VMA(2), VMA(3) );
		return 0;

/*
	case ENGINE_LOADCAMERA:
		return loadCamera(VMA(1));

	case ENGINE_STARTCAMERA:
		startCamera(args[1]);
		return 0;

	case ENGINE_GETCAMERAINFO:
		return getCameraInfo(args[1], VMA(2), VMA(3));
*/
	case ENGINE_GET_ENTITY_TOKEN:
		return re.GetEntityToken( VMA(1), args[2] );

	case ENGINE_R_INPVS:
		return re.inPVS( VMA(1), VMA(2) );

	// engine extensions
	case ENGINE_R_ADDREFENTITYTOSCENE2:
		re.AddRefEntityToScene( VMA(1), true );
		return 0;

	case ENGINE_R_ADDLINEARLIGHTTOSCENE:
		re.AddLinearLightToScene( VMA(1), VMA(2), VMF(3), VMF(4), VMF(5), VMF(6) );
		return 0;

	case ENGINE_R_FORCEFIXEDDLIGHTS:
		CL_ForceFixedDlights();
		return 0;

	case ENGINE_IS_RECORDING_DEMO:
		return clc.demorecording;

	case ENGINE_TRAP_GETVALUE:
		return CL_GetValue( VMA(1), args[2], VMA(3) );

	default:
		Com_Error( ERR_DROP, "Bad gameclient system trap: %ld", (long int) args[0] );
	}
	return 0;
}


/*
====================
CL_DllSyscall
====================
*/
static intptr_t QDECL CL_DllSyscall( intptr_t arg, ... ) {
#if !id386 || defined __clang__
	intptr_t	args[10]; // max.count for gameclient
	va_list	ap;
	int i;

	args[0] = arg;
	va_start( ap, arg );
	for (i = 1; i < ARRAY_LEN( args ); i++ )
		args[ i ] = va_arg( ap, intptr_t );
	va_end( ap );

	return CL_GameClientSystemCalls( args );
#else
	return CL_GameClientSystemCalls( &arg );
#endif
}


/*
====================
CL_InitGameClient

Should only be called by CL_StartHunkUsers
====================
*/
void CL_InitGameClient( void ) {
	const char			*info;
	const char			*mapname;
	int					t1, t2;
	vmInterpret_t		interpret;

	Cbuf_NestedReset();

	t1 = Sys_Milliseconds();

	// put away the console
	Con_Close();

	// find the current mapname
	info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cl.mapname, sizeof( cl.mapname ), "maps/%s.bsp", mapname );

	// allow vertex lighting for in-game elements
	re.VertexLighting( true );

	// load the dll or bytecode
	interpret = Cvar_VariableIntegerValue( "vm_gameclient" );

	gameClientVm = VM_Create( VM_GAMECLIENT, CL_GameClientSystemCalls, CL_DllSyscall, interpret );
	if ( !gameClientVm ) {
		Com_Error( ERR_DROP, "VM_Create on gameclient failed" );
	}
	cls.state = CA_LOADING;

	// init for this gamestate
	// use the lastExecutedServerCommand instead of the serverCommandSequence
	// otherwise server commands sent just before a gamestate are dropped
	VM_Call( gameClientVm, 3, CLIENT_INIT, clc.serverMessageSequence, clc.lastExecutedServerCommand, clc.clientNum );

	// reset any CVAR_CHEAT cvars registered by gameclient
	if ( !clc.demoplaying && !cl_connectedToCheatServer )
		Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	cls.state = CA_PRIMED;

	t2 = Sys_Milliseconds();

	Com_Printf( "CL_InitGameClient: %5.2f seconds\n", (t2-t1)/1000.0 );

	// have the renderer touch all its images, so they are present
	// on the card even if the driver does deferred loading
	re.EndRegistration();

	// make sure everything is paged in
	if (!Sys_LowPhysicalMemory()) {
		Com_TouchMemory();
	}

	// clear anything that got printed
	Con_ClearNotify ();

	// do not allow vid_restart for first time
	cls.lastVidRestart = Sys_Milliseconds();
}


/*
====================
CL_GameCommand

See if the current console command is claimed by the gameclient
====================
*/

bool CL_GameCommand( void ) {
	bool bRes;

	if ( !gameClientVm ) {
		return false;
	}

	bRes = (bool)VM_Call( gameClientVm, 0, CLIENT_CONSOLE_COMMAND );

	Cbuf_NestedReset();

	return bRes;
}


/*
=====================
CL_GameClientRendering
=====================
*/
void CL_GameClientRendering( stereoFrame_t stereo ) {
	VM_Call( gameClientVm, 3, CLIENT_DRAW_ACTIVE_FRAME, cl.serverTime, stereo, clc.demoplaying );
#ifdef DEBUG
	VM_Debug( 0 );
#endif
}


/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time approach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives with a rational
latency, which keeps the adjustment process framerate independent and
prevents massive overadjustment during times of significant packet loss
or bursted delayed packets.
=================
*/

#define	RESET_TIME	500

static void CL_AdjustTimeDelta( void ) {
	int		newDelta;
	int		deltaDelta;

	cl.newSnapshots = false;

	// the delta never drifts when replaying a demo
	if ( clc.demoplaying ) {
		return;
	}

	newDelta = cl.snap.serverTime - cls.realtime;
	deltaDelta = abs( newDelta - cl.serverTimeDelta );

	if ( deltaDelta > RESET_TIME ) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.snap.serverTime;	// FIXME: is this a problem for gameclient?
		cl.serverTime = cl.snap.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	} else if ( deltaDelta > 100 ) {
		// fast adjust, cut the difference in half
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<FAST> " );
		}
		cl.serverTimeDelta = ( cl.serverTimeDelta + newDelta ) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if ( com_timescale->value == 0 || com_timescale->value == 1 ) {
			if ( cl.extrapolatedSnapshot ) {
				cl.extrapolatedSnapshot = false;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
}


/*
==================
CL_FirstSnapshot
==================
*/
static void CL_FirstSnapshot( void ) {
	// ignore snapshots that don't have entities
	if ( cl.snap.snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}
	cls.state = CA_ACTIVE;

	// clear old game so we will not switch back to old mod on disconnect
	CL_ResetOldGame();

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	cl.oldServerTime = cl.snap.serverTime;

	clc.timeDemoBaseTime = cl.snap.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	// this is to allow scripting a timedemo to start right
	// after loading
	if ( cl_activeAction->string[0] ) {
		Cbuf_AddText( cl_activeAction->string );
		Cbuf_AddText( "\n" );
		Cvar_Set( "activeAction", "" );
	}

	Sys_BeginProfiling();
}


/*
==================
CL_AvgPing

Calculates Average Ping from snapshots in buffer. Used by AutoNudge.
==================
*/
static float CL_AvgPing( void ) {
	int ping[PACKET_BACKUP];
	int count = 0;
	int i, j, iTemp;
	float result;

	for ( i = 0; i < PACKET_BACKUP; i++ ) {
		if ( cl.snapshots[i].ping > 0 && cl.snapshots[i].ping < 999 ) {
			ping[count] = cl.snapshots[i].ping;
			count++;
		}
	}

	if ( count == 0 )
		return 0;

	// sort ping array
	for ( i = count - 1; i > 0; --i ) {
		for ( j = 0; j < i; ++j ) {
			if (ping[j] > ping[j + 1]) {
				iTemp = ping[j];
				ping[j] = ping[j + 1];
				ping[j + 1] = iTemp;
			}
		}
	}

	// use median average ping
	if ( (count % 2) == 0 )
		result = (ping[count / 2] + ping[(count / 2) - 1]) / 2.0f;
	else
		result = ping[count / 2];

	return result;
}


/*
==================
CL_TimeNudge

Returns either auto-nudge or cl_timeNudge value.
==================
*/
static int CL_TimeNudge( void ) {
	float autoNudge = cl_autoNudge->value;

	if ( autoNudge != 0.0f )
		return (int)((CL_AvgPing() * autoNudge) + 0.5f) * -1;
	else
		return cl_timeNudge->integer;
}


/*
==================
CL_SetGameClientTime
==================
*/
void CL_SetGameClientTime( void ) {
	bool demoFreezed;

	// getting a valid frame message ends the connection process
	if ( cls.state != CA_ACTIVE ) {
		if ( cls.state != CA_PRIMED ) {
			return;
		}
		if ( clc.demoplaying ) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if ( !clc.firstDemoFrameSkipped ) {
				clc.firstDemoFrameSkipped = true;
				return;
			}
			CL_ReadDemoMessage();
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = false;
			CL_FirstSnapshot();
		}
		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if ( !cl.snap.valid ) {
		Com_Error( ERR_DROP, "CL_SetGameClientTime: !cl.snap.valid" );
	}

	// allow pause in single player
	if ( sv_paused->integer && CL_CheckPaused() && com_sv_running->integer ) {
		// paused
		return;
	}

	if ( cl.snap.serverTime - cl.oldFrameServerTime < 0 ) {
		Com_Error( ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.snap.serverTime;

	// get our current view of time
	demoFreezed = clc.demoplaying && com_timescale->value == 0.0f;
	if ( demoFreezed ) {
		// \timescale 0 is used to lock a demo in place for single frame advances
		cl.serverTimeDelta -= cls.frametime;
	} else {
		// cl_timeNudge is a user adjustable cvar that allows more
		// or less latency to be added in the interest of better
		// smoothness or better responsiveness.
		cl.serverTime = cls.realtime + cl.serverTimeDelta - CL_TimeNudge();

		// guarantee that time will never flow backwards, even if
		// serverTimeDelta made an adjustment or cl_timeNudge was changed
		if ( cl.serverTime - cl.oldServerTime < 0 ) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		//if ( cls.realtime + cl.serverTimeDelta >= cl.snap.serverTime - 5 ) {
		if ( cls.realtime + cl.serverTimeDelta - cl.snap.serverTime >= -5 ) {
			cl.extrapolatedSnapshot = true;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}

	if ( !clc.demoplaying ) {
		return;
	}

	// if we are playing a demo back, we can just keep reading
	// messages from the demo file until the gameclient definitely
	// has valid snapshots to interpolate between

	// a timedemo will always use a deterministic set of time samples
	// no matter what speed machine it is run on,
	// while a normal demo may have different time samples
	// each time it is played back
	if ( com_timedemo->integer ) {
		if ( !clc.timeDemoStart ) {
			clc.timeDemoStart = Sys_Milliseconds();
		}
		clc.timeDemoFrames++;
		cl.serverTime = clc.timeDemoBaseTime + clc.timeDemoFrames * 50;
	}

	//while ( cl.serverTime >= cl.snap.serverTime ) {
	while ( cl.serverTime - cl.snap.serverTime >= 0 ) {
		// feed another message, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if ( cls.state != CA_ACTIVE ) {
			return; // end of demo
		}
	}
}
