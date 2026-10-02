#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_CHARS 255U

static int
known_section(const char *s)
{
        static const char *const names[] = {
                "OPTIONS", "OPERANDS", "ENVIRONMENT", "FILES",
                "EXIT STATUS", "SEE ALSO", "SUBCOMMANDS", "DIAGNOSTICS"
        };
        size_t i;

        for (i = 0U; i < sizeof(names) / sizeof(names[0]); ++i)
                if (strcmp(s, names[i]) == 0)
                        return 1;
        return 0;
}

static int
sixbit_source_char(unsigned char c)
{
        return c == '\t' || (c >= 040U && c <= 0137U);
}

static int
validate_case_escapes(const char *s)
{
        size_t i;

        for (i = 0U; s[i] != '\0'; ++i) {
                unsigned char c = (unsigned char)s[i];
                if (c >= 'a' && c <= 'z')
                        return 0;
                if (c != '@')
                        continue;
                if (s[i + 1U] == '\0')
                        return 0;
                if (s[i + 1U] == '@' ||
                    (s[i + 1U] >= 'A' && s[i + 1U] <= 'Z')) {
                        ++i;
                        continue;
                }
                if (s[i + 1U] == '?' && i >= 2U && s[i - 1U] == ' ')
                        continue;
                return 0;
        }
        return 1;
}

static void
trim_eol(char *s)
{
        size_t n = strlen(s);
        while (n != 0U && (s[n - 1U] == '\n' || s[n - 1U] == '\r'))
                s[--n] = '\0';
}

static int
fail(const char *path, unsigned long line, const char *why)
{
        fprintf(stderr, "%s:%lu: %s\n", path, line, why);
        return 1;
}

int
main(int argc, char **argv)
{
        FILE *fp;
        char buf[512];
        char section[64];
        char command[64];
        unsigned long line_no;
        unsigned int command_headings;
        int have_first;
        int rc;

        if (argc < 2 || argc > 3) {
                fprintf(stderr, "usage: sixmd-check FILE [COMMAND]\n");
                return 2;
        }
        fp = fopen(argv[1], "rb");
        if (fp == NULL) {
                perror(argv[1]);
                return 1;
        }
        section[0] = '\0';
        command[0] = '\0';
        line_no = 0UL;
        command_headings = 0U;
        have_first = 0;
        rc = 0;
        while (fgets(buf, sizeof(buf), fp) != NULL) {
                size_t n;
                size_t i;
                int level;
                const char *p;

                ++line_no;
                n = strlen(buf);
                if (n == sizeof(buf) - 1U && buf[n - 1U] != '\n') {
                        rc = fail(argv[1], line_no, "record exceeds input buffer");
                        break;
                }
                trim_eol(buf);
                n = strlen(buf);
                if (n > LINE_MAX_CHARS) {
                        rc = fail(argv[1], line_no, "record exceeds 255 characters");
                        break;
                }
                for (i = 0U; i < n; ++i)
                        if (!sixbit_source_char((unsigned char)buf[i])) {
                                rc = fail(argv[1], line_no,
                                    "character outside SIXBIT source repertoire");
                                break;
                        }
                if (rc != 0)
                        break;
                if (!validate_case_escapes(buf)) {
                        rc = fail(argv[1], line_no, "invalid SIXMD case escape");
                        break;
                }
                if (!have_first) {
                        have_first = 1;
                        if (strcmp(buf, "%SIXMD 1") != 0) {
                                rc = fail(argv[1], line_no,
                                    "first record must be %SIXMD 1");
                                break;
                        }
                        continue;
                }
                level = 0;
                while (level < 3 && buf[level] == '#')
                        ++level;
                if (level == 0 || buf[level] != ' ' ||
                    buf[level + 1] != '@' || buf[level + 2] != '?')
                        continue;
                p = &buf[level + 3];
                if (*p == '\0') {
                        rc = fail(argv[1], line_no, "empty semantic heading");
                        break;
                }
                if (level == 1) {
                        const char *dash;
                        size_t len;

                        ++command_headings;
                        dash = strstr(p, " - ");
                        if (dash == NULL || dash == p) {
                                rc = fail(argv[1], line_no,
                                    "command heading needs NAME - SUMMARY");
                                break;
                        }
                        len = (size_t)(dash - p);
                        if (len >= sizeof(command)) {
                                rc = fail(argv[1], line_no, "command name too long");
                                break;
                        }
                        memcpy(command, p, len);
                        command[len] = '\0';
                        section[0] = '\0';
                } else if (level == 2) {
                        if (!known_section(p)) {
                                rc = fail(argv[1], line_no,
                                    "unknown semantic section");
                                break;
                        }
                        if (strlen(p) >= sizeof(section)) {
                                rc = fail(argv[1], line_no, "section name too long");
                                break;
                        }
                        strcpy(section, p);
                } else if (section[0] == '\0') {
                        rc = fail(argv[1], line_no,
                            "semantic entry outside semantic section");
                        break;
                } else if (strcmp(section, "OPTIONS") == 0 && p[0] != '-') {
                        rc = fail(argv[1], line_no,
                            "option entry must begin with one-dash option");
                        break;
                }
        }
        if (rc == 0 && ferror(fp)) {
                perror(argv[1]);
                rc = 1;
        }
        fclose(fp);
        if (rc == 0 && !have_first)
                rc = fail(argv[1], 0UL, "empty manual");
        if (rc == 0 && command_headings != 1U)
                rc = fail(argv[1], 0UL,
                    "manual must contain exactly one semantic command heading");
        if (rc == 0 && argc == 3 && strcmp(command, argv[2]) != 0)
                rc = fail(argv[1], 0UL, "command heading does not match file target");
        return rc;
}
