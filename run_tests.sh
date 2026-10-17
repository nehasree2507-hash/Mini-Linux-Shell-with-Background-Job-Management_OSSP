#!/usr/bin/env bash
# Non-interactive test suite: feeds scripted input to mysh through a pipe
# and checks the output. Run from the repository root:  ./tests/run_tests.sh
SH=./mysh
pass=0; fail=0

check() {   # check <name> <input> <expected-substring>
  local name="$1" input="$2" expect="$3" out
  out=$(printf "$input" | timeout 15 $SH 2>&1)
  if grep -qF -- "$expect" <<<"$out"; then
    echo "PASS  $name"; pass=$((pass+1))
  else
    echo "FAIL  $name"; echo "      expected: $expect"; echo "      got: $out"; fail=$((fail+1))
  fi
}

check "T01 foreground command"          'echo hello\n'                         'hello'
check "T02 arguments"                   'echo a b c\n'                         'a b c'
check "T03 quoted argument"             'echo "x   y"\n'                       'x   y'
check "T04 unknown command"             'nosuchcmd\n'                          'No such file or directory'
check "T05 output redirect"             'echo data > /tmp/mysh_t.txt\ncat /tmp/mysh_t.txt\n' 'data'
check "T06 append redirect"             'echo one > /tmp/mysh_t2.txt\necho two >> /tmp/mysh_t2.txt\ncat /tmp/mysh_t2.txt\n' 'two'
check "T07 input redirect"              'echo in > /tmp/mysh_t3.txt\ncat < /tmp/mysh_t3.txt\n' 'in'
check "T08 cd builtin"                  'cd /tmp\npwd\n'                       '/tmp'
check "T09 cd bad dir"                  'cd /no/such/dir\n'                    'cd:'
check "T10 background launch"           'sleep 5 &\nkill %%1\n'                '[1] '
check "T11 jobs lists running job"      'sleep 5 &\njobs\nkill %%1\n'          'Running'
check "T12 jobs when empty"             'jobs\n'                               'No jobs.'
check "T13 background Done report"      'sleep 1 &\nsleep 2\njobs\n'           'Done'
check "T14 kill by job id"              'sleep 30 &\nkill %%1\nsleep 1\njobs\n' 'Terminated'
check "T15 kill -KILL (named signal)"   'sleep 30 &\nkill -KILL %%1\nsleep 1\njobs\n' 'Killed'
check "T16 job list shows command"             'sleep 30 &\njobs\n'                   'sleep 30'
check "T17 kill invalid job"            'kill %%9\n'                           'no such job'
check "T18 kill invalid signal"         'sleep 5 &\nkill -FOO %%1\nkill %%1\n' 'invalid signal'
check "T19 multiple bg jobs"            'sleep 5 &\nsleep 5 &\njobs\nkill %%1 %%2\n' '[2] '
check "T20 non-zero exit reported"      'sh -c "exit 3" &\nsleep 1\njobs\n'   'Exit 3'
check "T21 & must be last"              'echo a & b\n'                         "'&' must end"
check "T22 unterminated quote"          'echo "abc\n'                          'unterminated quote'
check "T23 help"                        'help\n'                               'built-ins'
check "T24 bg job does not block shell" 'sleep 3 &\necho prompt-still-works\nkill %%1\n' 'prompt-still-works'

# T25: no zombie children remain after background jobs finish
out=$(printf 'sleep 0.2 &\nsleep 0.2 &\nsleep 1\nsh -c "ps -o stat= --ppid \\$PPID"\n' | timeout 15 $SH 2>&1)
if grep -q 'Z' <<<"$out"; then echo "FAIL  T25 no zombies"; echo "$out"; fail=$((fail+1))
else echo "PASS  T25 no zombies"; pass=$((pass+1)); fi

# T26: exit terminates remaining jobs (no orphaned sleeps)
marker=$(( RANDOM % 1000 + 4000 ))
printf "sleep $marker &\nexit\n" | timeout 10 $SH >/dev/null 2>&1; sleep 0.5
if pgrep -f "sleep $marker" >/dev/null; then
  echo "FAIL  T26 exit terminates jobs"; fail=$((fail+1)); pkill -f "sleep $marker"
else echo "PASS  T26 exit terminates jobs"; pass=$((pass+1)); fi

echo "----"; echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
