# Rules

The rule file (‹gib.file› and whatever it includes) describes the static part
of the dependency graph; it is read from top to bottom, and is separated into
stanzas (delimited by blank lines). Lines starting with # are comments (these
don't count as blanks).

There are 3 types of stanzas:

 • template definitions which start with ‹def› and can contain anything
   except a ‹for› or a second ‹def›,
 • ‹for› stanzas which are repeated for each item in a given list,
 • normal stanzas, which can contain any statement except ‹for› and ‹def›.

A stanza may create at most a single node in the dependency graph. This
happens if it contains an ‹out› or ‹meta› statement (only one of these may
appear). Node-generating stanzas may additionally use ‹dep› statements and at
most one ‹cmd›, and may not use ‹inc› nor ‹src›.

There are also 4 types of nodes in the dependency graph: src, out, sys and
meta:

 • src nodes correspond to source files and are always dependency-less and
   have no commands attached either; these must all be created by the source
   manifest,
 • sys nodes refer to source files from outside the project, like system
   headers, the compiler and various other bits that enter the build… like
   src nodes, they never have any dependencies or commands; they can be
   declared statically (in this file) or dynamically (when they are
   discovered as dependencies during build),
 • out nodes must be declared statically, can have dependencies on any other
   node type except meta and are built by running commands given by their
   ‹cmd› statement,
 • meta nodes can depend on anything and can run commands, but must not
   create any (persistent) files (they mainly exist for convenience, e.g. to
   bundle a number of outputs under a single name, or to run non-build
   commands, like testsuites, though the latter might be better expressed as
   ‹out› nodes where the test output is dumped into the target file).

## Manifest

Source nodes are created by a manifest file (rarely by multiple manifest
files) that are declared using ‹src› statements. The paths given to ‹src› are
interpreted relative to the source directory. If the file does not exist, it
is looked up in the output directory. If still not found, an error is
reported.

## Variables

The environment is ‘immutable’ in the sense that when a variable is used, it
is frozen and cannot be changed anymore. Local variables work the same,
except they only exist within a single stanza (i.e. each can set them to a
different value). Shadowing a global variable is reported as an error. Like
nodes, global variables are timestamped. The ‹out›, ‹dep› and ‹cmd› commands
set/add to the identically named «local» variables:

 • ‹out foo› does an implicit ‹let out foo› and ‹let dep›,
 • ‹cmd foo bar› does an implicit ‹let cmd›, ‹add cmd foo›, ‹add cmd bar›,
 • ‹dep foo› does an implicit ‹add dep foo›).

Variables are set-list hybrids with strings as items: scalars and singletons
are the same thing. ‹add› appends a given value to the end of the ‘list’ and
inserts the value into the (sorted) set. If the same value is added multiple
times, it appears multiple times in the list but not in the set.

To use a variable, say $(foo); in ‹cmd› statements, each item in a list
becomes a single argument, while in ‹dep› and ‹add›, the statement is
effectively repeated for each item. Finally, in ‹out› (which names a node),
it is an error to use a non-singleton variable. Variable references may be
embedded in a string, in which case all possible substitutions are performed
to get a new list. Barring exceptional circumstances, all variable references
except one should be singletons. Incidentally, a variable reference is the
only way to get a space in the generated string. Or a dollar sign. No quoting
or escaping is provided. And newlines are entirely impossible.
