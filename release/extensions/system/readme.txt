System Extensions

Extensions extracted into this directory will be available from the
default "System" repository.

This allows extensions to be bundled with Blender outside of
user repositories.

Layout invariant:

- each bundled system extension must live directly under this directory
  as `release/extensions/system/<extension-id>`
- do not add an extra nested `system/` directory level for packaged
  extensions
- Blender already appends the repository module name (`system`) when it
  resolves the on-disk repo path
