# mysh

A small POSIX command-line shell written in C for Rutgers CS 214. It supports interactive use and batch scripts, with pipelines, file redirection, wildcard expansion, and built-in commands.

## Build and run

Requires a POSIX environment and a C compiler. Python 3 is needed for the test suite.

```sh
make
./mysh
./mysh tests/smoke.txt
make test
```

The interactive prompt shows the current directory and abbreviates the home directory with `~`. Batch mode reads a file argument or redirected standard input and omits prompts.

## Features

- Built-in `cd`, `pwd`, `which`, and `exit` commands
- Executable lookup in `/usr/local/bin`, `/usr/bin`, and `/bin`
- Multi-stage pipelines using `pipe()`, `fork()`, and `dup2()`
- Input redirection (`<`), output truncation (`>`), and append (`>>`)
- Single-asterisk wildcard expansion in the final pathname component
- Comments beginning with `#`
- Interactive reporting of nonzero exit codes and terminating signals

```sh
echo hello | cat | wc -c
pwd > location.txt
cat < location.txt
echo *.c
which echo cat
```

## Design

A shared input loop reads one complete line using `read()` before executing it. The parser builds an argument list and redirection targets for each pipeline stage. Standalone built-ins execute in the shell process so `cd` can change the shell's directory. Built-ins inside pipelines execute in child processes.

All pipeline stages start before the parent waits. Each process closes pipe ends it does not need, and the parent reaps every child. The final stage determines pipeline status. Batch-mode child input defaults to `/dev/null` unless explicitly redirected.

## Tests

`make test` runs 15 integration tests covering interactive and batch behavior, built-ins, wildcard expansion, redirection, syntax-error recovery, large pipelines, and exit handling. The suite was also run successfully on Rutgers iLab. See [TEST_PLAN.txt](TEST_PLAN.txt) for details.

## Scope

This is an educational shell. Quoting, escape sequences, variable expansion, background execution, and job control are not implemented. Wildcards support one asterisk in the final pathname component.

## Credits

Course project partners: Vishal Kanala and Tanisha Bohra.

The implementation and initial test suite were generated with assistance from ChatGPT and tested locally and on Rutgers iLab.
