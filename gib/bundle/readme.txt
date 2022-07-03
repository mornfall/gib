# synopsis

    gib [-c config] [target] […]

# description

‹gib› is a simple build system in the tradition of ‹make›:

 • it has a simple but powerful language for describing build graphs,
 • uses timestamps on source files to figure out what needs to be done,
 • no builtin support for particular programming languages.¹

Of course, it comes with some improvements over ‹make› (what would be the
point otherwise):

 • the language is simpler and more regular,
 • variables are true lists (not with significant whitespace),
 • outputs depend on the exact commands used to build them,
 • support for dynamic dependencies,
 • first-rate support for script-generated rules,
 • ‹gib› is much faster – comparable to ‹ninja›.

And some divergence in design choices:

 • build directories are always separate from sources,
 • no evaluation-time command execution (like ‹!=› in ‹make›),
 • build commands:
   ◦ are «not» passed to ‹sh› but ‹exec›'d as lists,
   ◦ run without access to the terminal,
   ◦ all outputs are logged by default, one log file per command,
   ◦ are not looked up in ‹PATH› (use absolute paths or ‹/usr/bin/env›).

The remainder of this document describes ‹gib› and its build description
language in more detail. If you want to see a quick and simple example before
diving in, check out ‹gib/bundle/boot.gib›.

¹ Unlike ‹make›, there are also no built-in rules for compiling C programs.
  The scope of macros is currently limited to a single file, but if/when that
  limitation is lifted, a small library of macros for building C/C++ projects
  might be bundled up with ‹gib›, to optionally include in your rule file.

# bundling

Since ‹gib› is very small and only relies on standard POSIX APIs and on C99,
it can be easily bundled with your project. The source code is canonically
under ‹gib/bundle› and you can make things transparent for your users by
creating a toplevel ‹makefile› with just ‹include gib/bundle/boot.mk› in it:
your users can then start the build by running

    $ make

which will first compile ‹gib› (takes about a second or two) and then run it,
forwarding any targets (so ‹make foo› will translate to ‹gib foo›). The
current version of ‹boot.mk› is tested with GNU make (by far the most common)
and OpenBSD make (hopefully also representative of other BSD variants).

# project structure

For simple builds, you can create a toplevel ‹gibfile› which contains all the
rules. In more complicated projects, you will generally want to split the
rules up and there will likely be additional build-related files. The
recommended practice is to put them under ‹gib›, with the toplevel rule file
being ‹gib/main›.

It is a good idea to conditionally include ‹gib/local› after setting your
configuration variables, so that users can conveniently override your
defaults, without tripping up version control.

# rule syntax

The rule file is a sequence of «stanzas» separated by blank lines and made of
statements. Each statement is exactly one line. If a stanza contains an ‹out›
statement, then it defines a node of the build graph. Variables declared using
the ‹let› statement are stanza-local. More about variables below. There are a
few syntactic categories that appear in the statement list below:

 • ‹{list}› – a list of strings with implicit word-splitting, giving a «list»
   of words; this splitting is done by the parser: only literal spaces in the
   source file qualify, before any variable expansion takes place,
 • ‹{string}› – a literal string, extending until the end of line, preserving
   whitespace exactly (including trailing spaces!),
 • ‹{path}› – like ‹{string}› but names a node of the build graph,
 • ‹{var}› – name of a variable – no spaces and none of ‹.:~$› are allowed.

The statements are as follows:

 • ‹set {var} [list]› sets a global variable (use ‹set= {var} {string}› to
   suppress implicit word splitting),
 • ‹let {var} [list]› same, but the variable is local (‹let=› also exists),
 • ‹add {var} [list]› append more values to an existing variable (also ‹add=›)
 • ‹sub {path}› include another rule file (use ‹sub?› to make the inclusion
   conditional, i.e. not an error if the path does not exist),
 • ‹src {var₁} {var₂} {path}› read a «manifest» (a list of source files),
   setting ‹var₁› to the list of all files listed in that manifest and ‹var₂›
   to the list of all directories (you can use ‹gib.findsrc› to generate the
   manifest, by hand or on the fly),
 • ‹out {path}› declare that this stanza describes a node of the build graph
   with the name given by ‹{path}›,
 • ‹dep {path}› declare a static dependency,
 • ‹cmd {list}› set the command that will be executed to build the node,
 • ‹def {var}› define a macro (see below),
 • ‹use {var}› use a previously defined macro.

Variables are expanded in ‹{list}›, ‹{string}› and ‹{path}› arguments, but not
in those of type ‹{var}›. Variable expansion is performed at read time and
variables, once they are so used, cannot be later changed. Attempting to use
an undefined variable is an error. Variable references take these forms:

  • ‹$(var)›: expands into the list stored in ‹var› (possibly empty),
  • ‹foo$(var)bar›: same, but prepend/append ‹foo› and ‹bar› to the expanded
    values, e.g. if ‹var› contains values ‹x› and ‹y›, the result will be a
    list with items ‹fooxbar› and ‹fooybar›,
  • ‹$(var.$key)›: ‹gib› does not allow ‘constructed variable names’ except in
    this special subscript form (see also below); ‹$key› must be a singleton
  • ‹$(var:pattern)›: keep only the items from ‹var› that match the pattern
    (beware, the resulting list will be sorted and deduplicated – on the
    upside, the prefix portion of the matching is very efficient, even when
    selecting a few items from lists with tens of thousands of them):
    ◦ if there are no ‹*› or ‹%› in the pattern, it is «prefix-matched»,
    ◦ if ‹*› or ‹%› are present, they both match an arbitrary substring
      (including an empty one), and the pattern is anchored at «both ends» (so
      that ‹$(var:src/*.cpp)› does what you imagine it should),
  • ‹$(var!pattern)›: same as above, but negated (remove matching items)
  • ‹$(var:pattern:replacement)›: filter like above, but replace the items
    from ‹var› with ‹replacement›, provided that:
    ◦ ‹$n› for ‹n› being a single digit is substituted for the string matched
      by the n-th ‹*› or ‹%› of the ‹pattern›,
    ◦ ‹*› matches greedily (longest possible match), as is usual, while ‹%›
      matches non-greedily (shortest possible match),
  • ‹$(var~regex)› and ‹$(var~regex~replace)› same as above, but with POSIX
    extended regular expressions, and with ‹$n› substituted for parenthesised
    capture expressions (numbered by their opening bracket); much less
    efficient than patterns [TODO not yet implemented].

Subscripted variables are declared as ‹set foo.bar› and they require that
‹foo› is itself an existing variable (usually set to be an empty list). Unlike
regular variables, using ‹foo.bar› that was never set is fine (as long as
‹foo› exists) and expands to an empty list.

# helper programs

To make certain things easier, ‹gib› comes bundled with a few additional
programs. To use them, put ‹sub gib/bundle/boot.gib› into your ‹gibfile›
(after configuration variables, but before your own rules), then simply use
them as ‹cmd gib.wrapcc …›. The helpers are:

 • ‹gib.findsrc› – generate manifest files on the fly, by traversing your
   source directory (for use with the ‹src› statement),
 • ‹gib.wrapcc› – run a given ‹cc› command and extract ‹#include› dependencies
   for use by ‹gib›,
 • ‹gib.nochange› – run a command but tell ‹gib› that the output has not
   changed between runs.

If you do not want to bundle ‹gib› with your project but still use those tools
(as presumably found on the user's ‹$PATH›), invoke them using ‹/usr/bin/env›.
