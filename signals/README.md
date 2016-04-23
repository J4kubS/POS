# Signals
Simple application with two processes demonstrating signal handling.

Once started, both the parent and the child processes will print their first letter.
After that, the parent will wait until the user presses enter key. If any process receives
`USR2` signal (`kill -s USR2 pid`), it will start printing letters from the beginning.

## How to build
```
$ gmake
```

## How to run
```
$ signals
```
