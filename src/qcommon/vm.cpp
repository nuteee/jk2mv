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
// vm.c -- virtual machine

/*


intermix code and data
symbol table

a dll has one imported function: VM_SystemCall
and one exported function: Perform


*/

#include "vm_local.h"


vm_t	*currentVM = NULL;
vm_t	*lastVM	= NULL;
int		vm_debugLevel;

#define MAX_APINUM	1000
qboolean vm_profileInclusive;

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

#define	MAX_VM		3
vm_t	vmTable[MAX_VM];


void VM_VmInfo_f( void );
void VM_VmProfile_f( void );



#if 0 // 64bit!
// converts a VM pointer to a C pointer and
// checks to make sure that the range is acceptable
void	*VM_VM2C( vmptr_t p, int length ) {
	return (void *)p;
}
#endif

void VM_Debug( int level ) {
	vm_debugLevel = level;
}

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
	Cvar_Get( "vm_cgame", "2", CVAR_ARCHIVE );	// !@# SHIP WITH SET TO 2
	Cvar_Get( "vm_game", "2", CVAR_ARCHIVE );	// !@# SHIP WITH SET TO 2
	Cvar_Get( "vm_ui", "2", CVAR_ARCHIVE );		// !@# SHIP WITH SET TO 2

	Cmd_AddCommand ("vmprofile", VM_VmProfile_f );
	Cmd_AddCommand ("vminfo", VM_VmInfo_f );

	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}


/*
===============
VM_ValueToSymbol

Assumes a program counter value
===============
*/
const char *VM_ValueToSymbol( vm_t *vm, int value ) {
	vmSymbol_t	*sym;
	static char		text[MAX_TOKEN_CHARS];

	if ( !vm->symbolTable ) {
		return "NO SYMBOLS";
	}

	sym = vm->symbolTable[value];

	if ( value == sym->symValue ) {
		return sym->symName;
	}

	Com_sprintf( text, sizeof( text ), "%s+%i", sym->symName, value - sym->symValue );

	return text;
}

/*
===============
VM_ValueToFunctionSymbol

For profiling, find the symbol behind this value
===============
*/
vmSymbol_t *VM_ValueToFunctionSymbol( vm_t *vm, int value ) {
	static vmSymbol_t	nullSym;

	if ( !vm->symbolTable ) {
		return &nullSym;
	}

	return vm->symbolTable[value];
}


/*
===============
VM_SymbolToValue
===============
*/
int VM_SymbolToValue( vm_t *vm, const char *symbol ) {
	vmSymbol_t	*sym;

	for ( sym = vm->symbols ; sym ; sym = sym->next ) {
		if ( !strcmp( symbol, sym->symName ) ) {
			return sym->symValue;
		}
	}
	return 0;
}


/*
=====================
VM_SymbolForCompiledPointer
=====================
*/
#if 0 // VM_ValueToSymbol takes compiled pointer as an argument now
const char *VM_SymbolForCompiledPointer( vm_t *vm, void *code ) {
	int			i;

	if ( code < (void *)vm->codeBase ) {
		return "Before code block";
	}
	if ( code >= (void *)(vm->codeBase + vm->codeLength) ) {
		return "After code block";
	}

	// find which original instruction it is after
	for ( i = 0 ; i < vm->codeLength ; i++ ) {
		if ( (void *)vm->instructionPointers[i] > code ) {
			break;
		}
	}
	i--;

	// now look up the bytecode instruction pointer
	return VM_ValueToSymbol( vm, i );
}
#endif



/*
===============
ParseHex
===============
*/
int	ParseHex( const char *text ) {
	int		value;
	int		c;

	value = 0;
	while ( ( c = *text++ ) != 0 ) {
		if ( c >= '0' && c <= '9' ) {
			value = value * 16 + c - '0';
			continue;
		}
		if ( c >= 'a' && c <= 'f' ) {
			value = value * 16 + 10 + c - 'a';
			continue;
		}
		if ( c >= 'A' && c <= 'F' ) {
			value = value * 16 + 10 + c - 'A';
			continue;
		}
	}

	return value;
}

/*
===============
VM_LoadSymbols
===============
*/
void VM_LoadSymbols( vm_t *vm ) {
	union {
		char	*c;
		void	*v;
	} mapfile;
	const char *text_p;
	const char *token;
	char	name[MAX_QPATH];
	char	symbols[MAX_QPATH];
	vmSymbol_t	**prev, *sym;
	int		count;
	int		value;
	int		chars;
	int		segment;
	int		numInstructions;
	int		i;

	if ( vm->compiled ) {
		return;
	}

#ifndef DEBUG_VM
	// don't load symbols if not developer
	if ( !com_developer->integer ) {
		return;
	}
#endif

	COM_StripExtension(vm->name, name, sizeof(name));
	Com_sprintf( symbols, sizeof( symbols ), "vm/%s.map", name );
	FS_ReadFile( symbols, &mapfile.v );
	if ( !mapfile.c ) {
		Com_Printf( "Couldn't load symbol file: %s\n", symbols );
		return;
	}

	numInstructions = vm->instructionCount;

	// 1000 for negative system calls
	vm->symbolTable = (vmSymbol_t **)Hunk_Alloc( (MAX_APINUM + vm->codeLength) * sizeof(*vm->symbolTable), h_high ) + MAX_APINUM;

	// parse the symbols
	text_p = mapfile.c;
	prev = &vm->symbols;
	count = 0;

	while ( 1 ) {
		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			break;
		}
		segment = ParseHex( token );
		if ( segment ) {
			COM_Parse( &text_p );
			COM_Parse( &text_p );
			continue;		// only load code segment values
		}

		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			Com_Printf( "WARNING: incomplete line at end of file\n" );
			break;
		}
		value = ParseHex( token );

		token = COM_Parse( &text_p );
		if ( !token[0] ) {
			Com_Printf( "WARNING: incomplete line at end of file\n" );
			break;
		}
		chars = (int)strlen( token );
		sym = (vmSymbol_t *)Hunk_Alloc( sizeof( *sym ) + chars, h_high );
		*prev = sym;
		prev = &sym->next;
		sym->next = NULL;

		// convert value from an instruction number to a code offset
		if ( value >= 0 && value < numInstructions ) {
			value = (int)vm->instructionPointers[value];
		}

		sym->symValue = value;
		Q_strncpyz( sym->symName, token, chars + 1 );

		// _stackStart and _stackEnd are marked as functions for some reason
		if (value < vm->codeLength) {
			vm->symbolTable[value] = sym;
		}

		count++;
	}

	// direct code to function who owns it
	static vmSymbol_t invalidSym;
	sym = &invalidSym;
	for ( i = -MAX_APINUM; i < 0; i++ ) {
		if ( vm->symbolTable[i] == NULL ) {
			vm->symbolTable[i] = sym;
		}
	}
	for ( i = 0; i < vm->codeLength; i++ ) {
		if ( vm->symbolTable[i] ) {
			sym = vm->symbolTable[i];
		}
		vm->symbolTable[i] = sym;
	}

	vm->numSymbols = count;
	Com_Printf( "%i symbols parsed from %s\n", count, symbols );
	FS_FreeFile( mapfile.v );
}

/*
============
VM_DllSyscall

Dlls will call this directly

 rcg010206 The horror; the horror.

  The syscall mechanism relies on stack manipulation to get its args.
   This is likely due to C's inability to pass "..." parameters to
   a function in one clean chunk. On PowerPC Linux, these parameters
   are not necessarily passed on the stack, so while (&arg[0] == arg)
   is true, (&arg[1] == 2nd function parameter) is not necessarily
   accurate, as arg's value might have been stored to the stack or
   other piece of scratch memory to give it a valid address, but the
   next parameter might still be sitting in a register.

  Quake's syscall system also assumes that the stack grows downward,
   and that any needed types can be squeezed, safely, into a signed int.

  This hack below copies all needed values for an argument to a
   array in memory, so that Quake can get the correct values. This can
   also be used on systems where the stack grows upwards, as the
   presumably standard and safe stdargs.h macros are used.

  As for having enough space in a signed int for your datatypes, well,
   it might be better to wait for DOOM 3 before you start porting.  :)

  The original code, while probably still inherently dangerous, seems
   to work well enough for the platforms it already works on. Rather
   than add the performance hit for those platforms, the original code
   is still in use there.

  For speed, we just grab 15 arguments, and don't worry about exactly
   how many the syscall actually needs; the extra is thrown away.

============
*/
intptr_t QDECL __attribute__((no_sanitize_address)) VM_DllSyscall( intptr_t arg, ... ) {
#if !id386 || defined __clang__
  // rcg010206 - see commentary above
  intptr_t args[MAX_VMSYSCALL_ARGS];
  size_t i;
  va_list ap;

  args[0] = arg;

  va_start(ap, arg);
  for (i = 1; i < ARRAY_LEN (args); i++)
	args[i] = va_arg(ap, intptr_t);
  va_end(ap);

  return currentVM->systemCall( args );
#else // original id code
	return currentVM->systemCall( &arg );
#endif
}


/*
=================
VM_LoadQVM

Load a .qvm file
=================
*/
vmHeader_t *VM_LoadQVM( vm_t *vm, qboolean alloc)
{
	int					dataLength;
	int					i;
	char				filename[MAX_QPATH];
	union {
		vmHeader_t	*h;
		void				*v;
	} header;

	// load the image
	Com_sprintf( filename, sizeof(filename), "vm/%s.qvm", vm->name );
	Com_Printf( "Loading vm file %s...\n", filename );

	FS_ReadFile(filename, &header.v);

	if ( !header.h ) {
		Com_Printf( "Failed.\n" );
		VM_Free( vm );

		Com_Printf(S_COLOR_YELLOW "Warning: Couldn't open VM file %s\n", filename);

		return NULL;
	}

	// show where the qvm was loaded from
	//FS_Which(filename, vm->searchPath);

	if( LittleLong( header.h->vmMagic ) == VM_MAGIC ) {
		// byte swap the header
		// sizeof( vmHeader_t ) - sizeof( int ) is the 1.32b vm header size
		for ( size_t i = 0 ; i < ( sizeof( vmHeader_t ) - sizeof( int ) ) / 4 ; i++ ) {
			((int *)header.h)[i] = LittleLong( ((int *)header.h)[i] );
		}

		// validate
		if ( header.h->bssLength < 0
			|| header.h->dataLength < 0
			|| header.h->litLength < 0
			|| header.h->codeLength <= 0 )
		{
			VM_Free(vm);
			FS_FreeFile(header.v);

			Com_Printf(S_COLOR_YELLOW "Warning: %s has bad header\n", filename);
			return NULL;
		}
	} else {
		VM_Free( vm );
		FS_FreeFile(header.v);

		Com_Printf(S_COLOR_YELLOW "Warning: %s does not have a recognisable "
				"magic number in its header\n", filename);
		return NULL;
	}

	// round up to next power of 2 so all data operations can
	// be mask protected
	dataLength = header.h->dataLength + header.h->litLength +
		header.h->bssLength;
	for ( i = 0 ; dataLength > ( 1 << i ) ; i++ ) {
	}
	dataLength = 1 << i;

	if(alloc)
	{
		// allocate zero filled space for initialized and uninitialized data
		vm->dataBase = (byte *)Hunk_Alloc(dataLength, h_high);
		vm->dataMask = dataLength - 1;
	}
	else
	{
		// clear the data, but make sure we're not clearing more than allocated
		if(vm->dataMask + 1 != dataLength)
		{
			VM_Free(vm);
			FS_FreeFile(header.v);

			Com_Printf(S_COLOR_YELLOW "Warning: Data region size of %s not matching after "
					"VM_Restart()\n", filename);
			return NULL;
		}

		Com_Memset(vm->dataBase, 0, dataLength);
	}

	// copy the intialized data
	Com_Memcpy( vm->dataBase, (byte *)header.h + header.h->dataOffset,
		header.h->dataLength + header.h->litLength );

	// byte swap the longs
	for ( i = 0 ; i < header.h->dataLength ; i += 4 ) {
		*(int *)(vm->dataBase + i) = LittleLong( *(int *)(vm->dataBase + i ) );
	}

	return header.h;
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation

We need to make sure that servers can access unpure QVMs (not contained in any pak)
even if the client is pure, so take "unpure" as argument.
=================
*/
vm_t *VM_Restart(vm_t *vm)
{
	vmHeader_t	*header;

	// DLL's can't be restarted in place
	if ( vm->dllHandle ) {
		char	name[MAX_QPATH];
		intptr_t	(*systemCall)( intptr_t *parms );
		qboolean mvOverride = vm->mvOverride;

		systemCall = vm->systemCall;
		Q_strncpyz( name, vm->name, sizeof( name ) );

		VM_Free( vm );

		vm = VM_Create( name, mvOverride, systemCall, VMI_NATIVE );
		return vm;
	}

	// load the image
	Com_Printf("VM_Restart()\n");

	if(!(header = VM_LoadQVM(vm, qfalse)))
	{
		Com_Error(ERR_DROP, "VM_Restart failed");
		return NULL;
	}

	// free the original file
	FS_FreeFile(header);

	return vm;
}

/*
================
VM_Create

If image ends in .qvm it will be interpreted, otherwise
it will attempt to load as a system dll
================
*/
vm_t *VM_Create( const char *module, qboolean mvOverride, intptr_t (*systemCalls)(intptr_t *), vmInterpret_t interpret ) {
	vm_t		*vm;
	vmHeader_t	*header;
	int			i, remaining;
	void *startSearch = NULL;

	if ( !module || !module[0] || !systemCalls ) {
		Com_Error( ERR_FATAL, "VM_Create: bad parms" );
	}

	remaining = Hunk_MemoryRemaining();

	// see if we already have the VM
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if (!Q_stricmp(vmTable[i].name, module)) {
			vm = &vmTable[i];
			return vm;
		}
	}

	// find a free vm
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if ( !vmTable[i].name[0] ) {
			break;
		}
	}

	if ( i == MAX_VM ) {
		Com_Error( ERR_FATAL, "VM_Create: no free vm_t" );
	}

	vm = &vmTable[i];

	Q_strncpyz(vm->name, module, sizeof(vm->name));

	if (interpret == VMI_NATIVE) {
		// try to load as a system dll
		Com_Printf("Loading library file %s.\n", vm->name);
		vm->dllHandle = Sys_LoadModuleLibrary(module, mvOverride, &vm->entryPoint, VM_DllSyscall);
		if (vm->dllHandle) {
			vm->systemCall = systemCalls;
			return vm;
		}

		if (mvOverride) {
			Com_Error(ERR_FATAL, "Failed loading library file: %s", vm->name);
		}

		Com_Printf("Failed to load library, looking for qvm.\n");
		interpret = VMI_COMPILED;
	}

	vm->searchPath = startSearch;
	if ((header = VM_LoadQVM(vm, qtrue)) == NULL) {
		return NULL;
	}

	vm->systemCall = systemCalls;

	// allocate space for the jump targets, which will be filled in by the compile/prep functions
	vm->instructionCount = header->instructionCount;
	vm->instructionPointers = (intptr_t *)Hunk_Alloc(vm->instructionCount * sizeof(*vm->instructionPointers), h_high);

	// copy or compile the instructions
	vm->codeLength = header->codeLength;

	vm->compiled = qfalse;

#ifdef NO_VM_COMPILED
	if(interpret >= VMI_COMPILED) {
		Com_Printf("Architecture doesn't have a bytecode compiler, using interpreter\n");
		interpret = VMI_BYTECODE;
	}
#else
	if(interpret != VMI_BYTECODE)
	{
		vm->compiled = qtrue;
		VM_Compile( vm, header );
	}
#endif
	// VM_Compile may have reset vm->compiled if compilation failed
	if (!vm->compiled)
	{
		VM_PrepareInterpreter( vm, header );
	}

	// free the original file
	FS_FreeFile( header );

	// load the map file
	VM_LoadSymbols( vm );

	// the stack is implicitly at the end of the image
	vm->programStack = vm->dataMask + 1;
	vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE;

	Com_Printf("%s loaded in %d bytes on the hunk\n", module, remaining - Hunk_MemoryRemaining());

	return vm;
}

/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

	if(!vm) {
		return;
	}

	if(vm->callLevel) {
		if(!forced_unload) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if(vm->destroy)
		vm->destroy(vm);

	if ( vm->dllHandle ) {
		Sys_UnloadModuleLibrary( vm->dllHandle );
		Com_Memset( vm, 0, sizeof( *vm ) );
	}
#if 0	// now automatically freed by hunk
	if ( vm->codeBase ) {
		Z_Free( vm->codeBase );
	}
	if ( vm->dataBase ) {
		Z_Free( vm->dataBase );
	}
	if ( vm->instructionPointers ) {
		Z_Free( vm->instructionPointers );
	}
#endif
	Com_Memset( vm, 0, sizeof( *vm ) );

	currentVM = NULL;
	lastVM = NULL;
}

void VM_Clear(void) {
	int i;
	for (i=0;i<MAX_VM; i++) {
		VM_Free(&vmTable[i]);
	}
}

void VM_Forced_Unload_Start(void) {
	forced_unload = 1;
}

void VM_Forced_Unload_Done(void) {
	forced_unload = 0;
}

void *VM_ArgPtr( intptr_t intValue ) {
	if ( !intValue ) {
		return NULL;
	}
	// currentVM is missing on reconnect
	if ( currentVM==NULL )
	  return NULL;

	if ( currentVM->entryPoint ) {
		return (void *)(currentVM->dataBase + intValue);
	}
	else {
		return (void *)(currentVM->dataBase + (intValue & currentVM->dataMask));
	}
}

void *VM_ArgArray( intptr_t intValue, intptr_t size, intptr_t num ) {
	// currentVM is missing on reconnect
	if ( !currentVM ) {
		return NULL;
	}
	if ( currentVM->entryPoint ) {
		return (void *) intValue;
	}
	if ( !intValue ) {
		return NULL;
	}

	// don't drop on overflow for compatibility reasons
	intValue &= currentVM->dataMask;

	// coming from a module so should be within int32_t
	// range. SysCalls need to handle negative values.
	assert( size <= INT32_MAX && num <= INT32_MAX );

	if ( (int64_t) num * size > currentVM->dataMask - intValue + 1 ) {
		Com_Error( ERR_DROP, "VM_ArgArray: memory overflow" );
	}

	return (void *)(currentVM->dataBase + intValue);
}

void *VM_ExplicitArgPtr( vm_t *vm, intptr_t intValue ) {
	if ( !intValue ) {
		return NULL;
	}

	// currentVM is missing on reconnect here as well?
	if ( currentVM==NULL )
	  return NULL;

	//
	if ( vm->entryPoint ) {
		return (void *)(vm->dataBase + intValue);
	}
	else {
		return (void *)(vm->dataBase + (intValue & vm->dataMask));
	}
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

intptr_t QDECL  __attribute__((no_sanitize_address)) VM_Call( vm_t *vm, int callnum, ... )
{
	vm_t	*oldVM;
	intptr_t r;
	size_t i;

	if(!vm || !vm->name[0])
		Com_Error(ERR_FATAL, "VM_Call with NULL vm");

	oldVM = currentVM;
	currentVM = vm;
	lastVM = vm;

	if ( vm_debugLevel ) {
	  Com_Printf( "VM_Call( %d )\n", callnum );
	}

	++vm->callLevel;
	// if we have a dll loaded, call it directly
	if ( vm->entryPoint ) {
		//rcg010207 -  see dissertation at top of VM_DllSyscall() in this file.
		int args[MAX_VMMAIN_ARGS-1];
		va_list ap;
		va_start(ap, callnum);
		for (i = 0; i < ARRAY_LEN(args); i++) {
			args[i] = va_arg(ap, int);
		}
		va_end(ap);

		r = vm->entryPoint( callnum,  args[0],  args[1],  args[2], args[3],
                            args[4],  args[5],  args[6], args[7],
                            args[8],  args[9], args[10], args[11]);
	} else {
#if ( id386 || idsparc ) && !defined __clang__ // calling convention doesn't need conversion in some cases
#ifndef NO_VM_COMPILED
		if ( vm->compiled )
			r = VM_CallCompiled( vm, (int*)&callnum );
		else
#endif
			r = VM_CallInterpreted( vm, (int*)&callnum );
#else
		struct {
			int callnum;
			int args[MAX_VMMAIN_ARGS-1];
		} a;
		va_list ap;

		a.callnum = callnum;
		va_start(ap, callnum);
		for (i = 0; i < ARRAY_LEN(a.args); i++) {
			a.args[i] = va_arg(ap, int);
		}
		va_end(ap);
#ifndef NO_VM_COMPILED
		if ( vm->compiled )
			r = VM_CallCompiled( vm, &a.callnum );
		else
#endif
			r = VM_CallInterpreted( vm, &a.callnum );
#endif
	}
	--vm->callLevel;

	if ( oldVM != NULL )
	  currentVM = oldVM;
	return r;
}

//=================================================================

static int QDECL VM_ProfileSort( const void *a, const void *b ) {
	const vmSymbol_t	*sa, *sb;

	sa = *(const vmSymbol_t * const *)a;
	sb = *(const vmSymbol_t * const *)b;

	if ( sa->profileCount < sb->profileCount ) {
		return -1;
	}
	if ( sa->profileCount > sb->profileCount ) {
		return 1;
	}

	return 0;
}

/*
==============
VM_VmProfile_f

==============
*/
void VM_VmProfile_f( void ) {
	vm_t		*vm = NULL;
	vmSymbol_t	**sorted, *sym;
	int			i;
	long		total;
	int			totalCalls;
	qboolean	printHelp = qfalse;
	qboolean	resetCounts = qfalse;
	qboolean	printAll = qfalse;

	if ( Cmd_Argc() >= 2 ) {
		const char *arg = Cmd_Argv(1);

		if ( !Q_stricmp(arg, "exclusive") ) {
			vm_profileInclusive = qfalse;
			resetCounts = qtrue;
			Com_Printf("Collecting exclusive function instruction counts...\n");
		} else if ( !Q_stricmp(arg, "inclusive") ) {
			vm_profileInclusive = qtrue;
			resetCounts = qtrue;
			Com_Printf("Collecting inclusive function instruction counts...\n");
		} else if ( !Q_stricmp(arg, "print") ) {
			if (Cmd_Argc() >= 3) {
				for ( i = 0; i < MAX_VM; i++ ) {
					if ( !Q_stricmp(Cmd_Argv(2), vmTable[i].name) ) {
						vm = &vmTable[i];
						break;
					}
				}
			} else {
				// pick first VM with symbols
				for ( i = 0; i < MAX_VM; i++ ) {
					if ( vmTable[i].numSymbols ) {
						vm = &vmTable[i];
						break;
					}
				}
			}
		} else {
			printHelp = qtrue;
		}
	} else {
		printHelp = qtrue;
	}

	if ( resetCounts ) {
		for ( i = 0; i < MAX_VM; i++ ) {
			for ( sym = vmTable[i].symbols ; sym ; sym = sym->next ) {
				sym->profileCount = 0;
				sym->callCount = 0;
				sym->caller = NULL;
			}
		}
		return;
	}

	if ( printHelp ) {
		Com_Printf("Usage: vmprofile exclusive        start collecting exclusive counts\n");
		Com_Printf("       vmprofile inclusive        start collecting inclusive counts\n");
		Com_Printf("       vmprofile print [vm]       print collected data\n");
		return;
	}

	if ( vm == NULL || vm->numSymbols <= 0 ) {
		Com_Printf("No symbols\n");
		return;
	}

	sorted = (vmSymbol_t **)Z_Malloc( vm->numSymbols * sizeof( *sorted ), TAG_VM, qtrue);
	sorted[0] = vm->symbols;
	total = sorted[0]->profileCount;
	totalCalls = sorted[0]->callCount;
	for ( i = 1 ; i < vm->numSymbols ; i++ ) {
		sorted[i] = sorted[i-1]->next;
		total += sorted[i]->profileCount;
		totalCalls += sorted[i]->callCount;
	}

	// assume everything is called from vmMain
	if ( vm_profileInclusive )
		total = VM_ValueToFunctionSymbol( vm, 0 )->profileCount;

	if ( total > 0 ) {
		qsort( sorted, vm->numSymbols, sizeof( *sorted ), VM_ProfileSort );

		Com_Printf( "%4s %12s %9s Function Name\n",
			vm_profileInclusive ? "Incl" : "Excl",
			"Instructions", "Calls" );

		// todo: collect associations for generating callgraphs
		fileHandle_t callgrind = FS_FOpenFileWrite( va("callgrind.out.%s", vm->name) );
		// callgrind header
		FS_Printf( callgrind,
			"events: VM_Instructions\n"
			"fl=vm/%s.qvm\n\n", vm->name );

		for ( i = 0 ; i < vm->numSymbols ; i++ ) {
			int		perc;

			sym = sorted[i];

			if (printAll || sym->profileCount != 0 || sym->callCount != 0) {
				perc = 100 * sym->profileCount / total;
				Com_Printf( "%3i%% %12li %9i %s\n", perc, sym->profileCount, sym->callCount, sym->symName );
			}

			FS_Printf(callgrind,
				"fn=%s\n"
				"0 %li\n\n",
				sym->symName, sym->profileCount);
		}

		FS_FCloseFile( callgrind );
	}

	Com_Printf("     %12li %9i total\n", total, totalCalls );

	Z_Free( sorted );
}

/*
==============
VM_VmInfo_f

==============
*/
void VM_VmInfo_f( void ) {
	vm_t	*vm;
	int		i;

	Com_Printf( "Registered virtual machines:\n" );
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		vm = &vmTable[i];
		if ( !vm->name[0] ) {
			break;
		}
		Com_Printf( "%s : ", vm->name );
		if ( vm->dllHandle ) {
			Com_Printf( "native\n" );
		} else {
			if (vm->compiled) {
				Com_Printf("compiled on load\n");
			} else {
				Com_Printf("interpreted\n");
			}
			Com_Printf("    code length : %7i\n", vm->codeLength);
			Com_Printf("    table length: %7i\n", vm->instructionCount * 4);
			Com_Printf("    data length : %7i\n", vm->dataMask + 1);
		}

		Com_Printf("    mvapi level : %i\n", vm->mvapilevel);
	}
}

/*
===============
VM_LogSyscalls

Insert calls to this while debugging the vm compiler
===============
*/
void VM_LogSyscalls( int *args ) {
	static	int		callnum;
	static	FILE	*f;

	if ( !f ) {
		f = fopen("syscalls.log", "w" );
	}
	callnum++;
	fprintf(f, "%i: %p (%i) = %i %i %i %i\n", callnum, (void*)(args - (int *)currentVM->dataBase),
		args[0], args[1], args[2], args[3], args[4] );
}

/*
=================
VM_BlockCopy
Executes a block copy operation within currentVM data space
=================
*/

void VM_BlockCopy(unsigned int dest, unsigned int src, size_t n)
{
	unsigned int dataMask = currentVM->dataMask;

	if ((dest & dataMask) != dest
	|| (src & dataMask) != src
	|| ((dest + n) & dataMask) != dest + n
	|| ((src + n) & dataMask) != src + n)
	{
		Com_Error(ERR_DROP, "OP_BLOCK_COPY out of range!");
	}

	Com_Memcpy(currentVM->dataBase + dest, currentVM->dataBase + src, n);
}

int	VM_MVAPILevel(const vm_t *vm) {
	return vm->mvapilevel;
}

void VM_SetMVAPILevel(vm_t *vm, int level) {
	vm->mvapilevel = level;
}
