/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef QUAKE_PROGS_H
#define QUAKE_PROGS_H

#include "pr_comp.h"	/* defs shared with qcc */
#include "progdefs.h"	/* generated by program cdefs */

typedef union eval_s
{
	string_t	string;
	float		_float;
	float		vector[3];
	func_t		function;
	int		_int;
	int		edict;
} eval_t;

#define	MAX_ENT_LEAFS	32
typedef struct edict_s
{
	qboolean	free;			/* don't modify directly, use ED_AddToFreeList/ED_RemoveFromFreeList */
	link_t		freechain;
	link_t		area;			/* linked to a division node or leaf */

	int		num_leafs;
	int		leafnums[MAX_ENT_LEAFS];

	entity_state_t	baseline;
	unsigned char	alpha;			/* johnfitz -- hack to support alpha since it's not part of entvars_t */
	qboolean	sendinterval;		/* johnfitz -- send time until nextthink to client for better lerp timing */

	float		freetime;		/* sv.time when the object was freed */
	entvars_t	v;			/* C exported fields from progs */

	/* other fields from progs come immediately after */
} edict_t;

#define	EDICT_FROM_AREA(l)	STRUCT_FROM_LINK(l,edict_t,area)

//============================================================================

#define MAX_BUILTINS		1280
typedef void (*builtin_t) (void);

typedef struct
{
	int		s;
	dfunction_t	*f;
} prstack_t;

typedef struct prhashtable_s
{
	int			capacity;
	const char	**strings;
	int			*indices;
} prhashtable_t;

typedef struct qcvm_s
{
	dprograms_t		*progs;
	dfunction_t		*functions;
	dstatement_t	*statements;
	float			*globals;	/* same as pr_global_struct */
	ddef_t			*fielddefs;	//yay reflection.

	int				edict_size;	/* in bytes */

	qboolean		alpha_supported; //johnfitz
	int				effects_mask; // only enable 2021 rerelease quad/penta dlights when applicable

	builtin_t		builtins[MAX_BUILTINS];
	int				numbuiltins;

	int				argc;

	qboolean		trace;
	dfunction_t		*xfunction;
	int				xstatement;

	unsigned short	crc;

	//was static inside pr_edict
	char			*strings;
	int				stringssize;
	const char		**knownstrings;
	int				maxknownstrings;
	int				numknownstrings;
	int				freeknownstrings;
	const char		**firstfreeknownstring; // free list (singly linked)
	ddef_t			*globaldefs;

	prhashtable_t	ht_fields;
	prhashtable_t	ht_functions;
	prhashtable_t	ht_globals;

	//originally defined in pr_exec, but moved into the switchable qcvm struct
#define	MAX_STACK_DEPTH		1024 /*was 64*/	/* was 32 */
	prstack_t		stack[MAX_STACK_DEPTH];
	int				depth;

#define	LOCALSTACK_SIZE		16384 /* was 2048*/
	int				localstack[LOCALSTACK_SIZE];
	int				localstack_used;

	//originally part of the sv_state_t struct
	//FIXME: put worldmodel in here too.
	double		time;
	int			num_edicts;
	int			max_edicts;
	link_t		free_edicts;		// linked list of free edicts
	edict_t		*edicts;			// can NOT be array indexed, because
									// edict_t is variable sized, but can
									// be used to reference the world ent
} qcvm_t;

extern	globalvars_t	*pr_global_struct;

extern qcvm_t *qcvm;
void PR_SwitchQCVM(qcvm_t *nvm);

void PR_Init (void);

void PR_ExecuteProgram (func_t fnum);
void PR_ClearProgs(qcvm_t *vm);
void PR_LoadProgs (void);

const char *PR_GetString (int num);
int PR_SetEngineString (const char *s);
int PR_AllocString (int bufferlength, char **ptr);

void PR_Profile_f (void);

edict_t *ED_Alloc (void);
void ED_Free (edict_t *ed);
void ED_ClearEdict (edict_t *e);

void ED_Print (edict_t *ed);
void ED_Write (FILE *f, edict_t *ed);
const char *ED_ParseEdict (const char *data, edict_t *ent);

void ED_WriteGlobals (FILE *f);
const char *ED_ParseGlobals (const char *data);

void ED_LoadFromFile (const char *data);

/*
#define EDICT_NUM(n)		((edict_t *)(sv.edicts+ (n)*pr_edict_size))
#define NUM_FOR_EDICT(e)	(((byte *)(e) - sv.edicts) / pr_edict_size)
*/
edict_t *EDICT_NUM(int);
int NUM_FOR_EDICT(edict_t*);

#define	NEXT_EDICT(e)		((edict_t *)( (byte *)e + qcvm->edict_size))

#define	EDICT_TO_PROG(e)	((byte *)e - (byte *)qcvm->edicts)
#define PROG_TO_EDICT(e)	((edict_t *)((byte *)qcvm->edicts + e))

#define	G_FLOAT(o)		(qcvm->globals[o])
#define	G_INT(o)		(*(int *)&qcvm->globals[o])
#define	G_EDICT(o)		((edict_t *)((byte *)qcvm->edicts+ *(int *)&qcvm->globals[o]))
#define G_EDICTNUM(o)		NUM_FOR_EDICT(G_EDICT(o))
#define	G_VECTOR(o)		(&qcvm->globals[o])
#define	G_STRING(o)		(PR_GetString(*(string_t *)&qcvm->globals[o]))
#define	G_FUNCTION(o)		(*(func_t *)&qcvm->globals[o])

#define	E_FLOAT(e,o)		(((float*)&e->v)[o])
#define	E_INT(e,o)		(*(int *)&((float*)&e->v)[o])
#define	E_VECTOR(e,o)		(&((float*)&e->v)[o])
#define	E_STRING(e,o)		(PR_GetString(*(string_t *)&((float*)&e->v)[o]))

extern	int		type_size[8];

extern	builtin_t	pr_basebuiltins[];
extern	int			pr_numbasebuiltins;

typedef struct extbuiltin_s
{
	const char	*name;
	builtin_t	func;
	int			number;
} extbuiltin_t;

extern extbuiltin_t	pr_extbuiltins[];
extern int			pr_numextbuiltins;

FUNC_NORETURN void PR_RunError (const char *error, ...) FUNC_PRINTF(1,2);
#ifdef __WATCOMC__
#pragma aux PR_RunError aborts;
#endif

void ED_PrintEdicts (void);
void ED_PrintNum (int ent);

eval_t *GetEdictFieldValue(edict_t *ed, const char *field);

#endif	/* QUAKE_PROGS_H */
