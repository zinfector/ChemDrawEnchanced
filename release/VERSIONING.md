# Extending automatic build adoption

The delivered manager automatically discovers detour RVAs and executable idle
thunks. Its validated layout/call profile remains build-specific. Removing the
hash guards would allow relocated or changed native code to execute with stale
object accesses; it is not a complete version-portability implementation.

For genuine adaptation, every patch needs a resolved symbol/layout contract:

```text
module identity and architecture
function identity, calling convention, parameter/return ABI
unique instruction/semantic anchors and required caller relationships
private globals: resolved address, type, read/write width and ownership
private fields: offset, type and owner-class identity
vtable identity and semantic slot mapping
caller-range and return-address predicates
dependency/rollback requirements
```

The next implementation step is an exhaustive Ghidra inventory of `fn(...)`,
`base + ...`, other module-relative arithmetic, `at(...)`, and `vf(...)` across
the shim. Detour-only inventory is insufficient. Convert these accesses into
typed entries in a `ResolvedBuild` context so runtime feature code never adds
reference-version addresses directly.

Use exports and decorated symbols first. For nonexports, use normalized
instruction sequences and independent anchors such as imported API references,
strings, RTTI names, callers, control-flow shape and field-access patterns. A
candidate must satisfy the complete patch contract, with no ambiguous match.
Function boundary/unwind information must agree with the resolved entry.

For data addresses, resolve RIP-relative operands from a verified owning
instruction rather than retaining their old displacement. For vtables, identify
the class using RTTI and inspect the relevant function slots. Resolve field
offsets from validated accessor/constructor use sites; check independently
against other accesses and expected object relationships. Compare call argument
and return handling with the reference ABI. Inferred Ghidra prototypes alone are
not reliable ABI certificates.

A practical first compatibility tier accepts moved code only when the entire
required contract remains equivalent. A changed layout or semantic algorithm
requires a new patch adapter. Fail that feature independently, disable dependent
features, and explain the unresolved contract in the GUI. A weighted similarity
score alone must never authorize a native write or detour.

Profiles should bind discovered mappings to module hashes and revalidate both
the disk images and loaded bytes before activation. Resolve everything before
creating hooks; enable hooks only after dependencies and rollback are ready.
Keep restoration independent of rediscovery by storing original bytes, file
identities and installed-state hashes at installation time.

The supplied project contains one reference build. It cannot establish that
the inferred layouts and algorithms hold across vendor updates. Automatic
cross-build installation is therefore deliberately disabled in this release.
