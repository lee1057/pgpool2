/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * parse_analyze.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parser.c,v 1.63 2004/12/31 22:00:27 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>
#include "pool_parser.h"
#include "pool_memory.h"
#include "gramparse.h"
#include "gram.h"
#include "parser.h"
#include "value.h"

List	   *parsetree;			/* result of parsing is left here */
jmp_buf    jmpbuffer;

static int	lookahead_token;	/* one-token lookahead */
static bool have_lookahead;		/* lookahead_token set? */
static YYSTYPE lookahead_yylval;	/* yylval for lookahead token */
static YYLTYPE lookahead_yylloc;	/* yylloc for lookahead token */


/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.
 */
List *
raw_parser(const char *str)
{
	int			yyresult;

	if (pool_memory == NULL)
		pool_memory = pool_memory_create();

	parsetree = NIL;			/* in case grammar forgets to set it */
	have_lookahead = false;

	scanner_init(str);
	parser_init();

	if (setjmp(jmpbuffer) != 0)
	{
		scanner_finish();
		return NIL; /* error */
	}
	else
	{
		yyresult = base_yyparse();

		scanner_finish();

		if (yyresult)				/* error */
			return NIL;
	}
	return parsetree;
}

void free_parser(void)
{
	pool_memory_delete(pool_memory, 1);
}

/*
 * Intermediate filter between parser and base lexer (base_yylex in scan.l).
 *
 * The filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.	We reduce these cases to one-token
 * lookahead by combining tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do it without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 */
int
filtered_base_yylex(void)
{
	int			cur_token;
	int			next_token;
	YYSTYPE		cur_yylval;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (have_lookahead)
	{
		cur_token = lookahead_token;
		base_yylval = lookahead_yylval;
		base_yylloc = lookahead_yylloc;
		have_lookahead = false;
	}
	else
		cur_token = base_yylex();

	/* Do we need to look ahead for a possible multiword token? */
	switch (cur_token)
	{
		case NULLS_P:
			/*
			 * NULLS FIRST and NULLS LAST must be reduced to one token
			 */
			cur_yylval = base_yylval;
			cur_yylloc = base_yylloc;
			next_token = base_yylex();
			switch (next_token)
			{
				case FIRST_P:
					cur_token = NULLS_FIRST;
					break;
				case LAST_P:
					cur_token = NULLS_LAST;
					break;
				default:
					/* save the lookahead token for next time */
					lookahead_token = next_token;
					lookahead_yylval = base_yylval;
					lookahead_yylloc = base_yylloc;
					have_lookahead = true;
					/* and back up the output info to cur_token */
					base_yylval = cur_yylval;
					base_yylloc = cur_yylloc;
					break;
			}
			break;

		case WITH:
			/*
			 * WITH CASCADED, LOCAL, or CHECK must be reduced to one token
			 *
			 * XXX an alternative way is to recognize just WITH_TIME and put
			 * the ugliness into the datetime datatype productions instead of
			 * WITH CHECK OPTION.  However that requires promoting WITH to a
			 * fully reserved word.  If we ever have to do that anyway
			 * (perhaps for SQL99 recursive queries), come back and simplify
			 * this code.
			 */
			cur_yylval = base_yylval;
			cur_yylloc = base_yylloc;
			next_token = base_yylex();
			switch (next_token)
			{
				case CASCADED:
					cur_token = WITH_CASCADED;
					break;
				case LOCAL:
					cur_token = WITH_LOCAL;
					break;
				case CHECK:
					cur_token = WITH_CHECK;
					break;
				default:
					/* save the lookahead token for next time */
					lookahead_token = next_token;
					lookahead_yylval = base_yylval;
					lookahead_yylloc = base_yylloc;
					have_lookahead = true;
					/* and back up the output info to cur_token */
					base_yylval = cur_yylval;
					base_yylloc = cur_yylloc;
					break;
			}
			break;

		default:
			break;
	}

	return cur_token;
}
