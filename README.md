# OSN Mini Project 1

This repository contains my implementation for Operating Systems and Networks Mini Project 1.

## C-Shell

The C-Shell is implemented in `c-shell/` and supports:

- Shell input, parsing, quoting and syntax errors
- `hop`, `reveal`, `peek` and `locate`
- Command execution, redirection and pipelines
- Sequential and background execution
- Job control with process groups, `activities`, `resume` and `ping`
- `spy` for inspecting open files through `/proc`
- `snoop` for tracing and summarizing Linux system calls using `ptrace`

Build from `c-shell/` with:

```bash
make all
./shell.out
```

## xv6

The `xv6/` directory contains the xv6 part of the project, including the scheduler-related work and supporting tests.

## AI Usage

AI tools were used as a programming aid for understanding requirements, discussing implementation approaches, debugging selected issues, and reviewing code. The final code was compiled and tested manually, and implementation decisions were checked against the project specification.

A concise record of the AI assistance used for this project is provided in `AI-usage.pdf`.
