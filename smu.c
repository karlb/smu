/* smu - simple markup
 * Copyright (C) <2007, 2008> Enno Boland <g s01 de>
 *               2019-2022 Karl Bartel <karl@karl.berlin>
 *               2022 bzt
 *
 * See LICENSE for further informations
 */
#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LENGTH(x)  sizeof(x)/sizeof(x[0])
#define ADDC(b,i)  if (i % BUFSIZ == 0) { b = realloc(b, (i + BUFSIZ) * sizeof(char)); if (!b) eprint("Malloc failed."); } b[i]

typedef int (*Parser)(const char *, const char *, int);
typedef struct {
	char *search;
	int process;
	char *before, *after;
} Tag;

static int docomment(const char *begin, const char *end, int newblock);   /* Parser for html-comments */
static int docodefence(const char *begin, const char *end, int newblock); /* Parser for code fences */
static int dohtml(const char *begin, const char *end, int newblock);      /* Parser for html */
static int dolineprefix(const char *begin, const char *end, int newblock);/* Parser for line prefix tags */
static int dolink(const char *begin, const char *end, int newblock);      /* Parser for links and images */
static int dolist(const char *begin, const char *end, int newblock);      /* Parser for lists */
static int dotable(const char *begin, const char *end, int newblock);     /* Parser for tables */
static int doparagraph(const char *begin, const char *end, int newblock); /* Parser for paragraphs */
static int doreplace(const char *begin, const char *end, int newblock);   /* Parser for simple replaces */
static int doshortlink(const char *begin, const char *end, int newblock); /* Parser for links and images */
static int dosurround(const char *begin, const char *end, int newblock);  /* Parser for surrounding tags */
static int dounderline(const char *begin, const char *end, int newblock); /* Parser for underline tags */
static void *ereallocz(void *p, size_t size);
static void hprint(const char *begin, const char *end);                   /* escapes HTML and prints it to output */
static void process(const char *begin, const char *end, int isblock);     /* Processes range between begin and end. */

/* list of parsers */
static Parser parsers[] = { dounderline, docomment, docodefence, dolineprefix,
	                    dolist, dotable, doparagraph, dosurround, dolink,
	                    doshortlink, dohtml, doreplace };
static int nohtml = 0;
static int in_paragraph = 0;

regex_t p_end_regex;  /* End of paragraph */

static Tag lineprefix[] = {
	{ "    ",       0,      "<pre><code>", "\n</code></pre>" },
	{ "\t",         0,      "<pre><code>", "\n</code></pre>" },
	{ ">",          2,      "<blockquote>", "</blockquote>" },
	{ "###### ",    1,      "<h6>",         "</h6>" },
	{ "##### ",     1,      "<h5>",         "</h5>" },
	{ "#### ",      1,      "<h4>",         "</h4>" },
	{ "### ",       1,      "<h3>",         "</h3>" },
	{ "## ",        1,      "<h2>",         "</h2>" },
	{ "# ",         1,      "<h1>",         "</h1>" },
	{ "- - -\n",    1,      "<hr />",       ""},
	{ "---\n",	1,	"<hr />",	""},
};

static Tag underline[] = {
	{ "=",          1,      "<h1>",         "</h1>\n" },
	{ "-",          1,      "<h2>",         "</h2>\n" },
};

static Tag surround[] = {
	{ "```",        0,      "<code>",       "</code>" },
	{ "``",         0,      "<code>",       "</code>" },
	{ "`",          0,      "<code>",       "</code>" },
	{ "___",        1,      "<strong><em>", "</em></strong>" },
	{ "***",        1,      "<strong><em>", "</em></strong>" },
	{ "__",         1,      "<strong>",     "</strong>" },
	{ "**",         1,      "<strong>",     "</strong>" },
	{ "_",          1,      "<em>",         "</em>" },
	{ "*",          1,      "<em>",         "</em>" },
};

static const char *replace[][2] = {
	/* Backslash escapes */
	{ "\\\\",       "\\" },
	{ "\\`",        "`" },
	{ "\\*",        "*" },
	{ "\\_",        "_" },
	{ "\\{",        "{" },
	{ "\\}",        "}" },
	{ "\\[",        "[" },
	{ "\\]",        "]" },
	{ "\\(",        "(" },
	{ "\\)",        ")" },
	{ "\\#",        "#" },
	{ "\\+",        "+" },
	{ "\\-",        "-" },
	{ "\\.",        "." },
	{ "\\!",        "!" },
	{ "\\\"",       "&quot;" },
	{ "\\$",        "$" },
	{ "\\%",        "%" },
	{ "\\&",        "&amp;" },
	{ "\\'",        "'" },
	{ "\\,",        "," },
	{ "\\-",        "-" },
	{ "\\.",        "." },
	{ "\\/",        "/" },
	{ "\\:",        ":" },
	{ "\\;",        ";" },
	{ "\\<",        "&lt;" },
	{ "\\>",        "&gt;" },
	{ "\\=",        "=" },
	{ "\\?",        "?" },
	{ "\\@",        "@" },
	{ "\\^",        "^" },
	{ "\\|",        "|" },
	{ "\\~",        "~" },
	/* HTML syntax symbols that need to be turned into entities */
	{ "<",          "&lt;" },
	{ ">",          "&gt;" },
	{ "&amp;",      "&amp;" },  /* Avoid replacing the & in &amp; */
	{ "&",          "&amp;" },
	/* Preserve newlines with two spaces before linebreak */
	{ "  \n",       "<br />\n" },
};

static const char *code_fence = "```";

void
eprint(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void end_paragraph(void) {
	if (in_paragraph) {
		fputs("</p>\n", stdout);
		in_paragraph = 0;
	}
}

int
docomment(const char *begin, const char *end, int newblock) {
	char *p;

	if (nohtml || strncmp("<!--", begin, 4))
		return 0;
	p = strstr(begin, "-->");
	if (!p || p + 3 >= end)
		return 0;
	fprintf(stdout, "%.*s\n", (int)(p + 3 - begin), begin);
	return (p + 3 - begin) * (newblock ? -1 : 1);
}

int
docodefence(const char *begin, const char *end, int newblock) {
	const char *p, *start, *stop, *lang_start, *lang_stop;
	unsigned int l = strlen(code_fence);

	if (!newblock)
		return 0;

	if (strncmp(begin, code_fence, l) != 0)
		return 0;

	/* Find start of content and read language string */
	start = begin + l;
	lang_start = start;
	while (start[0] != '\n')
		start++;
	lang_stop = start;
	start++;

	/* Find end of fence */
	p = start - 1;
	do {
		stop = p;
		p = strstr(p + 1, code_fence);
	} while (p && p[-1] == '\\');
	if (p && p[-1] != '\\')
		stop = p;

	/* No closing code fence means the rest of file is code (CommonMark) */
	if (!p)
		stop = end;

	/* Print output */
	if (lang_start == lang_stop) {
		fputs("<pre><code>", stdout);
	} else {
		fputs("<pre><code class=\"language-", stdout);
		hprint(lang_start, lang_stop);
		fputs("\">", stdout);
	}
	hprint(start, stop);
	fputs("</code></pre>\n", stdout);
	return -(stop - begin + l);
}

int
dohtml(const char *begin, const char *end, int newblock) {
	const char *p, *tag, *tagend;

	if (nohtml || begin + 2 >= end)
		return 0;
	p = begin;
	if (p[0] != '<' || !isalpha(p[1]))
		return 0;
	p++;
	tag = p;
	for (; isalnum(*p) && p < end; p++);
	tagend = p;
	if (p > end || tag == tagend)
		return 0;
	while ((p = strstr(p, "</")) && p < end) {
		p += 2;
		if (strncmp(p, tag, tagend - tag) == 0 && p[tagend - tag] == '>') {
			p++;
			fwrite(begin, sizeof(char), p - begin + tagend - tag, stdout);
			return p - begin + tagend - tag;
		}
	}
	p = strchr(tagend, '>');
	if (p) {
		fwrite(begin, sizeof(char), p - begin + 1, stdout);
		return p - begin + 1;
	}
	else
		return 0;
}

int
dolineprefix(const char *begin, const char *end, int newblock) {
	unsigned int i, j, l;
	char *buffer;
	const char *p;
	int consumed_input = 0;

	if (newblock)
		p = begin;
	else if (*begin == '\n') {
		p = begin + 1;
		consumed_input += 1;
	} else
		return 0;
	for (i = 0; i < LENGTH(lineprefix); i++) {
		l = strlen(lineprefix[i].search);
		if (end - p + 1 < l)
			continue;
		if (strncmp(lineprefix[i].search, p, l))
			continue;
		if (*begin == '\n')
			fputc('\n', stdout);

		/* All line prefixes add a block element. These are not allowed
		 * inside paragraphs, so we must end the paragraph first. */
		end_paragraph();

		fputs(lineprefix[i].before, stdout);
		if (lineprefix[i].search[l-1] == '\n') {
			fputc('\n', stdout);
			return l - 1 + consumed_input;
		}
		if (!(buffer = malloc(BUFSIZ)))
			eprint("Malloc failed.");
		buffer[0] = '\0';

		/* Collect lines into buffer while they start with the prefix */
		j = 0;
		while ((strncmp(lineprefix[i].search, p, l) == 0) && p + l < end) {
			p += l;

			/* Special case for blockquotes: optional space after > */
			if (lineprefix[i].search[0] == '>' && *p == ' ') {
				p++;
			}

			while (p < end) {
				ADDC(buffer, j) = *p;
				j++;
				if (*(p++) == '\n')
					break;
			}
		}

		/* Skip empty lines in block */
		while (*(buffer + j - 1) == '\n') {
			j--;
		}

		ADDC(buffer, j) = '\0';
		if (lineprefix[i].process)
			process(buffer, buffer + strlen(buffer), lineprefix[i].process >= 2);
		else
			hprint(buffer, buffer + strlen(buffer));
		puts(lineprefix[i].after);
		free(buffer);
		return -(p - begin);
	}
	return 0;
}

int
dolink(const char *begin, const char *end, int newblock) {
	int img, len, sep, parens_depth = 1;
	const char *desc, *link, *p, *q, *descend, *linkend;
	const char *title = NULL, *titleend = NULL;

	if (*begin == '[')
		img = 0;
	else if (strncmp(begin, "![", 2) == 0)
		img = 1;
	else
		return 0;
	p = desc = begin + 1 + img;
	if (!(p = strstr(desc, "](")) || p > end)
		return 0;
	for (q = strstr(desc, "!["); q && q < end && q < p; q = strstr(q + 1, "!["))
		if (!(p = strstr(p + 1, "](")) || p > end)
			return 0;
	descend = p;
	link = p + 2;

	/* find end of link while handling nested parens */
	q = link;
	while (parens_depth) {
		if (!(q = strpbrk(q, "()")) || q > end)
			return 0;
		if (*q == '(')
			parens_depth++;
		else
			parens_depth--;
		if (parens_depth && q < end)
			q++;
	}

	if ((p = strpbrk(link, "\"'")) && p < end && q > p) {
		sep = p[0]; /* separator: can be " or ' */
		title = p + 1;
		/* strip trailing whitespace */
		for (linkend = p; linkend > link && isspace(*(linkend - 1)); linkend--);
		for (titleend = q - 1; titleend > link && isspace(*(titleend)); titleend--);
		if (titleend < title || *titleend != sep) {
			return 0;
		}
	}
	else {
		linkend = q;
	}

	/* Links can be given in angular brackets */
	if (*link == '<' && *(linkend - 1) == '>') {
		link++;
		linkend--;
	}

	len = q + 1 - begin;
	if (img) {
		fputs("<img src=\"", stdout);
		hprint(link, linkend);
		fputs("\" alt=\"", stdout);
		hprint(desc, descend);
		fputs("\" ", stdout);
		if (title && titleend) {
			fputs("title=\"", stdout);
			hprint(title, titleend);
			fputs("\" ", stdout);
		}
		fputs("/>", stdout);
	}
	else {
		fputs("<a href=\"", stdout);
		hprint(link, linkend);
		fputs("\"", stdout);
		if (title && titleend) {
			fputs(" title=\"", stdout);
			hprint(title, titleend);
			fputs("\"", stdout);
		}
		fputs(">", stdout);
		process(desc, descend, 0);
		fputs("</a>", stdout);
	}
	return len;
}

int
dolist(const char *begin, const char *end, int newblock) {
	unsigned int i, j, indent, run, isblock, start_number;
	const char *p, *q, *num_start;
	char *buffer = NULL;
	char marker = '\0';  /* Bullet symbol or \0 for unordered lists */

	isblock = 0;
	if (newblock)
		p = begin;
	else if (*begin == '\n')
		p = begin + 1;
	else
		return 0;
	q = p;
	if (*p == '-' || *p == '*' || *p == '+') {
		marker = *p;
	} else {
		num_start = p;
		for (; p < end && *p >= '0' && *p <= '9'; p++);
		if (p >= end || ((*p != '.') && (*p != ')')))
			return 0;
		start_number = atoi(num_start);
	}
	p++;
	if (p >= end || !(*p == ' ' || *p == '\t'))
		return 0;

	end_paragraph();

	for (p++; p != end && (*p == ' ' || *p == '\t'); p++);
	indent = p - q;
	buffer = ereallocz(buffer, BUFSIZ);
	if (!newblock)
		fputc('\n', stdout);

	if (marker) {
		fputs("<ul>\n", stdout);
	} else if (start_number == 1) {
		fputs("<ol>\n", stdout);
	} else {
		printf("<ol start=\"%d\">\n", start_number);
	}
	run = 1;
	for (; p < end && run; p++) {
		for (i = 0; p < end && run; p++, i++) {
			if (*p == '\n') {
				if (p + 1 == end)
					break;
				else {
					/* Handle empty lines */
					for (q = p + 1; (*q == ' ' || *q == '\t') && q < end; q++);
					if (*q == '\n') {
						ADDC(buffer, i) = '\n';
						i++;
						run = 0;
						isblock++;
						p = q;
					}
				}
				q = p + 1;
				j = 0;
				if (marker && *q == marker)
					j = 1;
				else {
					for (; q + j != end && q[j] >= '0' && q[j] <= '9' && j < indent; j++);
					if (q + j == end)
						break;
					if (j > 0 && ((q[j] == '.') || (q[j] == ')')))
						j++;
					else
						j = 0;
				}
				if (q + indent < end)
					for (; (q[j] == ' ' || q[j] == '\t') && j < indent; j++);
				if (j == indent) {
					ADDC(buffer, i) = '\n';
					i++;
					p += indent;
					run = 1;
					if (*q == ' ' || *q == '\t')
						p++;
					else
						break;
				}
				else if (j < indent)
					run = 0;
			}
			ADDC(buffer, i) = *p;
		}
		ADDC(buffer, i) = '\0';
		fputs("<li>", stdout);
		process(buffer, buffer + i, isblock > 1 || (isblock == 1 && run));
		fputs("</li>\n", stdout);
	}
	fputs(marker ? "</ul>\n" : "</ol>\n", stdout);
	free(buffer);
	p--;
	while (*(--p) == '\n');
	return -(p - begin + 1);
}

int
dotable(const char *begin, const char *end, int newblock) {
	/* table state */
	static signed char intable, inrow, incell;
	static unsigned long int calign;
	static const char *align_table[] = {
		"",
		" style=\"text-align: left\"",
		" style=\"text-align: right\"",
		" style=\"text-align: center\"",
	};

	const char *p;
	int i, l = (int)sizeof(calign) * 4;

	if(*begin != '|')
		return 0;
	if (intable == 2) { /* in alignment row, skip it. */
		++intable;
		for (p = begin; p < end && *p != '\n'; ++p);
		return p - begin + 1;
	}
	if(inrow && (begin + 1 >= end || begin[1] == '\n')) {       /* close cell and row and if ends, table too */
		fprintf(stdout, "</t%c></tr>", inrow == -1 ? 'h' : 'd');
		if (inrow == -1)
			intable = 2;
		inrow = 0;
		if(end - begin <= 2 || begin[2] == '\n') {
			intable = 0;
			fputs("\n</table>\n", stdout);
		}
		return 1;
	}

	if(!intable) {                                              /* open table */
		intable = 1; inrow = -1; incell = 0; calign = 0;
		for (p = begin; p < end && *p != '\n'; ++p);
		if(*p == '\n') { /* load alignment from 2nd line */
			for(i = -1, ++p; p < end && *p != '\n'; p++) {
				if(*p == '|') {
					i++;
					do { p++; } while(p < end && (*p == ' ' || *p == '\t'));
					if(i < l && *p == ':')
						calign |= 1ul << (i * 2);
					if (*p == '\n')
						break;
				} else if(i < l && *p == ':') {
					calign |= 1ul << (i * 2 + 1);
				}
			}
		}
		fputs("<table>\n<tr>", stdout);
	}
	if(!inrow) {                                                /* open row */
		inrow = 1; incell = 0;
		fputs("<tr>", stdout);
	}
	if(incell)                                                  /* close cell */
		fprintf(stdout, "</t%c>", inrow == -1 ? 'h' : 'd');
	l = incell < l ? (calign >> (incell * 2)) & 3 : 0;          /* open cell */
	fprintf(stdout, "<t%c%s>", inrow == -1 ? 'h' : 'd', align_table[l]);
	incell++;
	for(p = begin + 1; p < end && *p == ' '; p++);
	return p - begin;
}

int
doparagraph(const char *begin, const char *end, int newblock) {
	const char *p;
	regmatch_t match;

	if (!newblock)
		return 0;
	if (regexec(&p_end_regex, begin + 1, 1, &match, 0)) {
		p = end;
	} else {
		p = begin + 1 + match.rm_so;
	}

	fputs("<p>", stdout);
	in_paragraph = 1;
	process(begin, p, 0);
	end_paragraph();

	return -(p - begin);
}

int
doreplace(const char *begin, const char *end, int newblock) {
	unsigned int i, l;

	for (i = 0; i < LENGTH(replace); i++) {
		l = strlen(replace[i][0]);
		if (end - begin < l)
			continue;
		if (strncmp(replace[i][0], begin, l) == 0) {
			fputs(replace[i][1], stdout);
			return l;
		}
	}
	return 0;
}

int
doshortlink(const char *begin, const char *end, int newblock) {
	const char *p, *c;
	int ismail = 0;

	if (*begin != '<')
		return 0;
	for (p = begin + 1; p != end; p++) {
		switch (*p) {
		case ' ':
		case '\t':
		case '\n':
			return 0;
		case '#':
		case ':':
			ismail = -1;
			break;
		case '@':
			if (ismail == 0)
				ismail = 1;
			break;
		case '>':
			if (ismail == 0)
				return 0;
			fputs("<a href=\"", stdout);
			if (ismail == 1) {
				/* mailto: */
				fputs("&#x6D;&#x61;i&#x6C;&#x74;&#x6F;:", stdout);
				for (c = begin + 1; *c != '>'; c++)
					fprintf(stdout, "&#%u;", *c);
				fputs("\">", stdout);
				for (c = begin + 1; *c != '>'; c++)
					fprintf(stdout, "&#%u;", *c);
			}
			else {
				hprint(begin + 1, p);
				fputs("\">", stdout);
				hprint(begin + 1, p);
			}
			fputs("</a>", stdout);
			return p - begin + 1;
		}
	}
	return 0;
}

int
dosurround(const char *begin, const char *end, int newblock) {
	unsigned int i, l;
	const char *p, *start, *stop;

	for (i = 0; i < LENGTH(surround); i++) {
		l = strlen(surround[i].search);
		if (end - begin < 2*l || strncmp(begin, surround[i].search, l) != 0)
			continue;
		start = begin + l;
		p = start;
		do {
			stop = p;
			p = strstr(p + 1, surround[i].search);
		} while (p && p[-1] == '\\');
		if (!p || p[-1] == '\\')  /* No unescaped closing marker found */
			continue;
		stop = p;
		if (!stop || stop < start || stop >= end)
			continue;
		fputs(surround[i].before, stdout);

		/* Single space at start and end are ignored */
		if (start[0] == ' ' && stop[-1] == ' ' && start < stop - 1) {
			start++;
			stop--;
			l++;
		}

		if (surround[i].process)
			process(start, stop, 0);
		else
			hprint(start, stop);
		fputs(surround[i].after, stdout);
		return stop - start + 2 * l;
	}
	return 0;
}

int
dounderline(const char *begin, const char *end, int newblock) {
	unsigned int i, j, l;
	const char *p;

	if (!newblock)
		return 0;
	p = begin;
	for (l = 0; p + l != end && p[l] != '\n'; l++);
	p += l + 1;
	if (l == 0)
		return 0;
	for (i = 0; i < LENGTH(underline); i++) {
		for (j = 0; p + j < end && p[j] != '\n' && p[j] == underline[i].search[0]; j++);
		if (j >= 3) {
			fputs(underline[i].before, stdout);
			if (underline[i].process)
				process(begin, begin + l, 0);
			else
				hprint(begin, begin + l);
			fputs(underline[i].after, stdout);
			return -(j + p - begin);
		}
	}
	return 0;
}

void *
ereallocz(void *p, size_t size) {
	void *res;
	res = realloc(p, size);
	if (!res)
		eprint("realloc: %zu bytes\n", size);
	return res;
}

void
hprint(const char *begin, const char *end) {
	const char *p;

	for (p = begin; p != end; p++) {
		if (*p == '&')
			fputs("&amp;", stdout);
		else if (*p == '"')
			fputs("&quot;", stdout);
		else if (*p == '>')
			fputs("&gt;", stdout);
		else if (*p == '<')
			fputs("&lt;", stdout);
		else
			fputc(*p, stdout);
	}
}

void
process(const char *begin, const char *end, int newblock) {
	const char *p;
	int affected;
	unsigned int i;

	for (p = begin; p < end;) {
		if (newblock)
			while (*p == '\n')
				if (++p == end)
					return;

		for (i = 0; i < LENGTH(parsers); i++)
			if ((affected = parsers[i](p, end, newblock)))
				break;
		if (affected)
			p += abs(affected);
		else
			fputc(*p++, stdout);

		/* Don't print single newline at end */
		if (p + 1 == end && *p == '\n')
			return;

		if (p[0] == '\n' && p + 1 != end && p[1] == '\n')
			newblock = 1;
		else
			newblock = affected < 0;
	}
}

int
main(int argc, char *argv[]) {
	char *buffer = NULL;
	int s, i;
	unsigned long len, bsize;
	FILE *source = stdin;

	regcomp(&p_end_regex, "(\n\n|(^|\n)```)", REG_EXTENDED);

	for (i = 1; i < argc; i++) {
		if (!strcmp("-v", argv[i]))
			eprint("simple markup %s (C) Enno Boland\n",VERSION);
		else if (!strcmp("-n", argv[i]))
			nohtml = 1;
		else if (argv[i][0] != '-')
			break;
		else if (!strcmp("--", argv[i])) {
			i++;
			break;
		}
		else
			eprint("Usage %s [-n] [file]\n -n escape html strictly\n", argv[0]);
	}
	if (i < argc && !(source = fopen(argv[i], "r")))
		eprint("Cannot open file `%s`\n",argv[i]);
	bsize = 2 * BUFSIZ;
	buffer = ereallocz(buffer, bsize);
	len = 0;
	while ((s = fread(buffer + len, 1, BUFSIZ, source))) {
		len += s;
		if (BUFSIZ + len + 1 > bsize) {
			bsize += BUFSIZ;
			if (!(buffer = realloc(buffer, bsize)))
				eprint("realloc failed.");
		}
	}
	buffer[len] = '\0';
	process(buffer, buffer + len, 1);
	fclose(source);
	free(buffer);
	return EXIT_SUCCESS;
}
