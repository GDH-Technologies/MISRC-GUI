#ifndef MISRC_CAPTURE_CLI_H
#define MISRC_CAPTURE_CLI_H

/*
 * Headless CLI capture entry points shared with the GUI binary.
 *
 * misrc_capture.c is compiled into the GUI executable (non-Android) as a
 * static library with `-Dmain=misrc_capture_main`, so the GUI binary can run
 * as the full misrc_capture CLI when capture args are present, without opening
 * a window. This header declares the two symbols the GUI links against.
 *
 * On Android there is no terminal/CLI, so the static library is not built and
 * these declarations are absent; the GUI main() guards its routing accordingly.
 */

#if !defined(__ANDROID__)

/* Run the full misrc_capture CLI capture flow (getopt, device open, capture,
 * file writing, progress). Returns the CLI process exit code. */
int misrc_capture_main(int argc, char **argv);

/* Print the full misrc_capture CLI usage/options to stderr (non-exiting). */
void misrc_capture_print_usage(void);

#endif /* !__ANDROID__ */

#endif /* MISRC_CAPTURE_CLI_H */
