/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_isc_dhcp.c
 * @brief Read ISC DHCP configuration files
 *
 * @copyright 2019 The FreeRADIUS server project
 * @copyright 2019 Alan DeKok <aland@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module.h>
#include <freeradius-devel/server/rad_assert.h>

#include <freeradius-devel/server/map_proc.h>

typedef struct rlm_isc_dhcp_info_t rlm_isc_dhcp_info_t;

/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct {
	char const		*name;
	char const		*filename;
	bool			debug;
	rlm_isc_dhcp_info_t	*head;
} rlm_isc_dhcp_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{ FR_CONF_OFFSET("filename", FR_TYPE_FILE_INPUT | FR_TYPE_REQUIRED | FR_TYPE_NOT_EMPTY, rlm_isc_dhcp_t, filename) },
	{ FR_CONF_OFFSET("debug", FR_TYPE_BOOL, rlm_isc_dhcp_t, debug) },
	CONF_PARSER_TERMINATOR
};

#define IDEBUG if (state->debug) DEBUG

typedef struct rlm_isc_dhcp_str_t {
	char		*name;
	int		len;
} rlm_isc_dhcp_str_t;

/*
 *	A struct that holds information about the thing we parsed.
 */
struct rlm_isc_dhcp_info_t {
	char const		*name;
	int			argc;
	char			**argv;
	rlm_isc_dhcp_info_t	*child;
	rlm_isc_dhcp_info_t	*next;
};

typedef struct rlm_isc_dhcp_tokenizer_t {
	FILE		*fp;
	char const	*filename;
	int		lineno;

	int		braces;
	bool		semicolon;
	bool		eof;
	bool		allow_eof;
	bool		debug;

	char		*buffer;
	size_t		bufsize;
	char		*ptr;

	rlm_isc_dhcp_str_t token;

	rlm_isc_dhcp_info_t	*head;
	rlm_isc_dhcp_info_t	**last;
} rlm_isc_dhcp_tokenizer_t;

typedef int (*rlm_isc_dhcp_parse_t)(rlm_isc_dhcp_tokenizer_t *state, rlm_isc_dhcp_info_t *info);

typedef struct rlm_isc_dhcp_cmd_t {
	char const		*name;
	rlm_isc_dhcp_parse_t	parse;
	int			max_argc;
} rlm_isc_dhcp_cmd_t;

static const rlm_isc_dhcp_cmd_t top_keywords[];
static int read_file(TALLOC_CTX *ctx, char const *filename, bool debug, rlm_isc_dhcp_info_t **head);
static int parse_section(TALLOC_CTX *ctx, rlm_isc_dhcp_tokenizer_t *state, rlm_isc_dhcp_info_t *info);

static int refill(rlm_isc_dhcp_tokenizer_t *state)
{
	char *p;

	if (state->eof) return 0;

	/*
	 *	We've hit EOL, refill the buffer with text.
	 */
	if (!*state->ptr) state->ptr = state->buffer;

redo:
	state->lineno++;
	if (!fgets(state->ptr, (state->buffer + state->bufsize) - state->ptr, state->fp)) {
		if (feof(state->fp)) {
			state->eof = true;
			return 0;
		}

		return -1;
	}

	/*
	 *	Skip leading spaces
	 */
	p = state->ptr;
	while (isspace((int) *p)) p++;

	/*
	 *	The line is all spaces, OR we've hit a comment.  Go
	 *	get more data.
	 */
	if (!*p || (*p == '#')) goto redo;

	/*
	 *	Point to the first non-space data.
	 */
	state->ptr = p;

	return 1;
}

/*
 *	Note that this function *destroys* the input buffer.  So if
 *	you need to read two tokens, you have to save the first one
 *	somewhere *outside* of the input buffer.
 */
static int read_token(rlm_isc_dhcp_tokenizer_t *state, FR_TOKEN hint, bool semicolon, bool allow_rcbrace)
{
	char *p;
	int lineno;

redo:
	/*
	 *	If the buffer is empty, re-fill it.
	 */
	if (!*state->ptr) {
		int rcode;
		
		/*
		 *	Read the next line into the start of the
		 *	buffer.
		 */
		state->ptr = state->buffer;

		rcode = refill(state);
		if (rcode < 0) return rcode;

		if (rcode == 0) {
			if (!state->allow_eof) {
				fr_strerror_printf("Failed reading %s line %d: Unexpected EOF",
						   state->filename, state->lineno);
				return -1;
			}

			return 0;
		}

	} else {
		/*
		 *	The previous token may have ended on a space
		 *	or semi-colon.  We skip those characters
		 *	before looking for the next token.
		 */
		while (isspace((int) *state->ptr) || (*state->ptr == ';')) state->ptr++;

		if (!*state->ptr) goto redo;
	}

	/*
	 *	Start looking for the next token from where we left
	 *	off last time.
	 */
	state->token.name = state->ptr;
	state->semicolon = false;

	/*
	 *	The "skip spaces at the end" may mangle this.
	 */
	lineno = state->lineno;

	for (p = state->token.name; *p != '\0'; p++) {
		/*
		 *	"end of word" character.  It might be allowed
		 *	here, or it might not be.
		 */
		if (*p == ';') {
			if (!semicolon) {
				fr_strerror_printf("Syntax error in %s at line %d: Unexpected ';'",
						   state->filename, state->lineno);
				return -1;			
			}

			state->ptr = p;
			state->semicolon = true;
			break;
		}

		/*
		 *	These are also "end of word" markers.
		 *
		 *	Allow them as single character tokens if
		 *	they're the first thing we saw.
		 */
		if ((*p == '{') || (*p == '}')) {
			if (p == state->token.name) p++;

			state->ptr = p;
			break;
		}

		/*
		 *	If we find a comment, we ignore everything
		 *	until the end of the line.
		 */
		if (*p == '#') {
			*p = '\0';
			state->ptr = p;

			/*
			 *	Nothing here, get more text
			 */
			if (state->token.name == state->ptr) goto redo;
			break;
		}

		/*
		 *	Be nice and eat trailing spaces, too.
		 */
		if (isspace((int) *p)) {
			state->ptr = p;
			char *start = p;

		skip_spaces:
			while (isspace((int) *state->ptr)) state->ptr++;
			
			/*
			 *	If we ran out of text on this line,
			 *	re-fill the buffer.  Note that
			 *	refill() also takes care of
			 *	suppressing blank lines and comments.
			 */
			if (!state->eof && !*state->ptr) {
				int rcode;

				state->ptr = start;

				rcode = refill(state);
				if (rcode < 0) return -1;
				goto skip_spaces;
			}

			/*
			 *	Set the semicolon flag as a "peek
			 *	ahead", so that the various other
			 *	parsers don't need to check it.
			 */
			if (*state->ptr == ';') state->semicolon = true;

			break;
		}
	}

	state->token.len = p - state->token.name;

	if (state->token.len >= 256) {
		fr_strerror_printf("Token too large");
		return -1;
	}

	/*
	 *	Double-check the token against what we were expected
	 *	to read.
	 */
	if (hint == T_LCBRACE) {
		if (*state->token.name != '{') {
			fr_strerror_printf("Failed reading %s line %d - Missing '{'",
					   state->filename, lineno);
			return -1;
		}

		state->braces++;
		return 1;
	}

	if (hint == T_RCBRACE) {
		if (*state->token.name != '}') {
			fr_strerror_printf("Failed reading %s line %d - Missing '}'",
					   state->filename, lineno);
			return -1;
		}

		state->braces--;
		return 1;
	}

	if (*state->token.name == '}') {
		if (!allow_rcbrace) {
			fr_strerror_printf("Failed reading %s line %d - Unexpected '}'",
					   state->filename, lineno);
			return -1;
		}

		state->braces--;
		return 1;
	}

	if ((hint == T_BARE_WORD) || (hint == T_DOUBLE_QUOTED_STRING)) {
		if (*state->token.name == '{') {
			fr_strerror_printf("Failed reading %s line %d - Unexpected '{'",
					   state->filename, lineno);
			return -1;
		}
	}


	return 1;
}

static int match_subword(rlm_isc_dhcp_tokenizer_t *state, char const *cmd, rlm_isc_dhcp_info_t *info)
{
	int rcode, type;
	bool semicolon = false;
	char *p;
	char const *q = cmd;
	char const *next;
	char type_name[64];
	fr_value_box_t box;

	while (isspace((int) *q)) q++;

	if (!*q) return -1;	/* internal error */

	next = q;
	while (*next && !isspace((int) *next)) next++;
	if (!*next) semicolon = true;

	if (islower((int) *q)) {
		char const *start = q;

		rcode = read_token(state, T_BARE_WORD, semicolon, false);
		if (rcode <= 0) return rcode;

		/*
		 *	Look for a verbatim word.
		 */
		for (p = state->token.name; p < (state->token.name + state->token.len); p++, q++) {
			if (*p != *q) {
			fail:
				fr_strerror_printf("Expected '%s', not '%.*s'",
						   start, state->token.len, state->token.name);
				return -1;
			}
		}

		/*
		 *	Matched all of 'q', we're done.
		 */
		if (!*q) {
			IDEBUG("... WORD %.*s ", state->token.len, state->token.name);
			return 1;
		}

		/*
		 *	Matched all of this word of 'q', but there's
		 *	more.  Recurse.
		 */
		if (isspace((int) *q)) {
			return match_subword(state, q, info);
		}

		/*
		 *	Matched all of 'p', but there's more 'q'.  Fail.
		 *
		 *	e.g. got "foo", but expected "food".
		 */
		goto fail;
	}

	/*
	 *	SECTION must be the last thing in the command
	 */
	if (strcmp(q, "SECTION") == 0) {
		rcode = read_token(state, T_LCBRACE, false, false);
		if (rcode <= 0) return rcode;

		rcode = parse_section(info, state, info);
		if (rcode < 0) return rcode;

		/*
		 *	Empty sections are allowed.
		 */

		q += 7;
		if (*q != '\0') return -1; /* internal error */

		return 2;	/* SECTION */
	}

	p = type_name;
	while (*q && !isspace((int) *q)) {
		*(p++) = tolower((int) *(q++));
	}

	type = fr_str2int(fr_value_box_type_names, type_name, -1);
	if (type < 0) {
		fr_strerror_printf("Unknown data type %s", cmd);
		return -1;
	}

	rcode = read_token(state, T_DOUBLE_QUOTED_STRING, semicolon, false);
	if (rcode <= 0) return rcode;

	IDEBUG("... DATA %.*s ", state->token.len, state->token.name);

	rcode = fr_value_box_from_str(info, &box, (fr_type_t *) &type, NULL,
				      state->token.name, state->token.len, 0, false);
	if (rcode < 0) return rcode;

	info->argv[info->argc++] = talloc_strndup(info, state->token.name, state->token.len);

	/*
	 *	No more data, return OK.
	 */
	if (!*next) return 1;

	/*
	 *	Keep matching more things
	 */
	return match_subword(state, next, info);
}

/*
 *	include FILENAME ;
 */
static int include_filename(TALLOC_CTX *ctx, rlm_isc_dhcp_tokenizer_t *state, char const *name, rlm_isc_dhcp_info_t **last)
{
	int rcode;
	char *p, pathname[8192];

	if ((name[0] == '/') ||
	    ((name[0] == '.') && (name[1] == '.'))) {
		fr_strerror_printf("Error in file %s at line %d: invalid (insecure) filename",
				   state->filename, state->lineno);
		return -1;
	}

	IDEBUG("include %s ;", name);

	p = strrchr(state->filename, '/');
	if (p) {
		strlcpy(pathname, state->filename, sizeof(pathname));
		p = pathname + (p - state->filename) + 1;
	}

	strlcpy(p, name, sizeof(pathname) - (p - pathname));

	rcode = read_file(ctx, pathname, state->debug, last);
	if (rcode < 0) return rcode;

	return 1;
}


static const rlm_isc_dhcp_cmd_t include_keyword[] = {
	{ "include STRING", NULL, 1},
	{ NULL, NULL }
};

static int match_keyword(TALLOC_CTX *ctx, rlm_isc_dhcp_tokenizer_t *state, rlm_isc_dhcp_cmd_t const *tokens, rlm_isc_dhcp_info_t **last)
{
	int i;
	rlm_isc_dhcp_info_t *info;

	for (i = 0; tokens[i].name != NULL; i++) {
		char const *q;
		char *p;
		bool semicolon;
		int rcode;

		p = state->token.name;

		/*
		 *	We've gone past the name and not found it.
		 *	Oops.
		 */
		if (*p < tokens[i].name[0]) break;

		/*
		 *	Not yet reached the correct name, don't do a
		 *	full strcmp()
		 */
		if (*p > tokens[i].name[0]) continue;

		q = tokens[i].name;

		while (p < (state->token.name + state->token.len)) {
			if (*p != *q) break;

			p++;
			q++;
		}

		/*
		 *	Not a match, go to the next token in the list.
		 */
		if (p < (state->token.name + state->token.len)) continue;

		semicolon = true; /* default to always requiring this */

		IDEBUG("... TOKEN %.*s ", state->token.len, state->token.name);

		info = talloc_zero(ctx, rlm_isc_dhcp_info_t);
		if (tokens[i].max_argc) {
			info->argv = talloc_zero_array(info, char *, tokens[i].max_argc);
		}

		/*
		 *	Remember which command we parsed.
		 */
		info->name = tokens[i].name;

		/*
		 *	There's more to this command,
		 *	go parse that, too.
		 */
		if (isspace((int) *q)) {
			if (state->semicolon) goto unexpected;

			rcode = match_subword(state, q, info);
			if (rcode <= 0) return rcode;

			/*
			 *	SUBSECTION must be at the end
			 */
			if (rcode == 2) semicolon = false;
		}

		/*
		 *	*q must be empty at this point.
		 */
		if (!semicolon && state->semicolon) {
		unexpected:
			fr_strerror_printf("Syntax error in %s at line %d: Unexpected ';'",
					   state->filename, state->lineno);
			talloc_free(info);
			return -1;
		}

		if (semicolon && !state->semicolon) {
			fr_strerror_printf("Syntax error in %s at line %d: Missing ';'",
					   state->filename, state->lineno);
			talloc_free(info);
			return -1;
		}

		/*
		 *	Call the "parse" function which should do
		 *	validation, etc.
		 */
		if (tokens[i].parse) {
			rcode = tokens[i].parse(state, info);
			if (rcode <= 0) {
				talloc_free(info);
				return rcode;
			}
		}

		/*
		 *	Add the parsed structure to the tail of the
		 *	current list.  Note that we can only add ONE
		 *	command at a time.
		 */
		*last = info;

		/*
		 *	It's a match, and it's OK.
		 */
		return 1;
	}

	/*
	 *	As a special case, match "include filename".  Which
	 *	doesn't get put into the command structure, but
	 *	instead mashes all of the structs in place.
	 */
	if ((state->token.len == 7) && (strncmp(state->token.name, "include", 7) == 0)) {
		int rcode;

		rcode = match_keyword(ctx, state, include_keyword, last);
		if (rcode <= 0) return rcode;

		info = *last;
		*last = NULL;

		rcode = include_filename(ctx, state, info->argv[0], last);
		talloc_free(info);
		if (rcode <= 0) return 0;

		return 1;
	}

	/*
	 *	No match.
	 */
	return 0;
}

static const rlm_isc_dhcp_cmd_t section_commands[] = {
	{ "adandon-lease-time INTEGER", NULL, 1},
	{ "adaptive-lease-time-threshold INTEGER", NULL, 1},
	{ "always-broadcast BOOL", NULL, 1},
	{ "fixed-address STRING", NULL, 1},
	{ "hardware ethernet ETHER", NULL, 1},
	{ NULL, NULL }
};

static int parse_section(TALLOC_CTX *ctx, rlm_isc_dhcp_tokenizer_t *state, rlm_isc_dhcp_info_t *info)
{
	int rcode;

	IDEBUG("{");

	while (true) {
		rlm_isc_dhcp_info_t **last = &(info->child);

		rcode = read_token(state, T_BARE_WORD, true, true);
		if (rcode <= 0) return rcode;

		/*
		 *	End of section is allowed here.
		 */
		if (*state->token.name == '}') break;

		rcode = match_keyword(ctx, state, section_commands, last);
		if (rcode <= 0) return rcode;

		rad_assert(*last != NULL);
		while (*last) last = &((*last)->next);
	}

	IDEBUG("}");

	return 1;
}


static const rlm_isc_dhcp_cmd_t top_keywords[] = {
	{ "host STRING SECTION", NULL, 1},
	{ NULL, NULL }
};

/*
 *	Match a top-level keyword
 */
static int match_top_keyword(TALLOC_CTX *ctx, rlm_isc_dhcp_tokenizer_t *state, rlm_isc_dhcp_cmd_t const *tokens,
			     rlm_isc_dhcp_info_t **last)
{
	int rcode;

	/*
	 *	Fill the buffer, and grab the length of the first word
	 *	in it.  As a hack, we allow for terminal words here,
	 *	and then check them later.
	 */
	state->allow_eof = true;
	rcode = read_token(state, T_BARE_WORD, true, false);
	if (rcode <= 0) return rcode;

	if (!isalpha((int) *state->token.name)) {
		fr_strerror_printf("Unexpected text '%.*s' in file %s line %d",
				   state->token.len, state->token.name,
				   state->filename, state->lineno);
		return -1;
	}

	rcode = match_keyword(ctx, state, tokens, last);
	if (rcode < 0) return rcode;

	if (rcode == 0) {
		fr_strerror_printf("Unknown keyword '%.*s' in file %s line %d",
				   state->token.len, state->token.name,
				   state->filename, state->lineno);
		return -1;
	}

	/*
	 *	Don't allow EOF when parsing blocks of text.
	 */
	state->allow_eof = false;
	return 1;
}

static int read_file(TALLOC_CTX *ctx, char const *filename, bool debug, rlm_isc_dhcp_info_t **head)
{
	FILE *fp;
	char buffer[8192];
	rlm_isc_dhcp_tokenizer_t state;
	rlm_isc_dhcp_info_t **last;

	/*
	 *	Read the file line by line.
	 *
	 *	The configuration file format is based off of
	 *	keywords, so we write a simple parser to check that.
	 */
	fp = fopen(filename, "r");
	if (!fp) {
		fr_strerror_printf("Error opening filename %s: %s", filename, fr_syserror(errno));
		return -1;
	}

	memset(&state, 0, sizeof(state));
	state.fp = fp;
	state.filename = filename;
	state.buffer = buffer;
	state.bufsize = sizeof(buffer);
	state.lineno = 0;

	state.braces = 0;
	state.ptr = buffer;

	state.head = NULL;
	last = &state.head;

	state.debug = debug;

	/*
	 *	Tell the state machine that the buffer is empty.
	 */
	*state.ptr = '\0';

	while (true) {
		int rcode;

		/*
		 *	This will automatically re-fill the buffer,
		 *	and find a matching token.
		 */
		rcode = match_top_keyword(ctx, &state, top_keywords, last);
		if (rcode < 0) {
			fclose(fp);
			return -1;
		}

		if (rcode == 0) {
			break;
		}

		while (*last) last = &((*last)->next);
	}

	// @todo - check that we actually did something

	fclose(fp);

	*head = state.head;

	if (!state.head) return 0;

	return 1;
}


/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int mod_bootstrap(void *instance, CONF_SECTION *conf)
{
	int rcode;
	rlm_isc_dhcp_t *inst = instance;
	
	inst->name = cf_section_name2(conf);
	if (!inst->name) inst->name = cf_section_name1(conf);

	rcode = read_file(inst, inst->filename, inst->debug, &inst->head);
	if (rcode < 0) {
		cf_log_err(conf, "%s", fr_strerror());
		return -1;
	}

	if (rcode == 0) {
		cf_log_err(conf, "No configuration read from %s", inst->filename);
		return -1;
	}

	return -1;
}


extern rad_module_t rlm_isc_dhcp;
rad_module_t rlm_isc_dhcp = {
	.magic		= RLM_MODULE_INIT,
	.name		= "isc_dhcp",
	.type		= 0,
	.inst_size	= sizeof(rlm_isc_dhcp_t),
	.config		= module_config,
	.bootstrap	= mod_bootstrap,
};
