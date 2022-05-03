if type c99 > /dev/null 2>&1; then
    exec c99 "$@"
elif type clang > /dev/null 2>&1; then
    exec clang -std=c99 "$@"
elif type gcc > /dev/null 2>&1; then
    exec gcc -std=c99 "$@"
else
    exec cc "$@" # hope for the best
fi
