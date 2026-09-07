"""User-facing immutability helpers.

This module re-exports the public API from the internal C extension
`_immutable`, keeping the programmer-facing surface in Python.
"""

from __future__ import annotations

import _immutable as _c

freeze = _c.freeze
is_frozen = _c.is_frozen
set_freezable = _c.set_freezable
get_freezable = _c.get_freezable
unset_freezable = _c.unset_freezable
NotFreezableError = _c.NotFreezableError
ImmutableModule = _c.ImmutableModule
FREEZABLE_YES = _c.FREEZABLE_YES
FREEZABLE_NO = _c.FREEZABLE_NO
FREEZABLE_EXPLICIT = _c.FREEZABLE_EXPLICIT
FREEZABLE_PROXY = _c.FREEZABLE_PROXY
InterpreterLocal = _c.InterpreterLocal
SharedField = _c.SharedField

# FIXME(immutable): For the longest time we used the name `isfrozen`
# without the underscore. This keeps the function name for now, but
# aliases it to `is_frozen`
isfrozen = is_frozen


# Artifact[Benchmarking]: The implementation of immutability related decorators
def freezable(cls):
    """Class decorator: mark a class as always freezable."""
    set_freezable(cls, FREEZABLE_YES)
    return cls


def unfreezable(cls):
    """Class decorator: mark a class as never freezable."""
    set_freezable(cls, FREEZABLE_NO)
    return cls


def explicitlyFreezable(cls):
    """Class decorator: mark a class as freezable only when passed directly to freeze()."""
    set_freezable(cls, FREEZABLE_EXPLICIT)
    return cls


def frozen(cls):
    """Class decorator: make a class freezable, then freeze it."""
    set_freezable(cls, FREEZABLE_YES)
    freeze(cls)
    return cls


class FreezabilityOverride:
    """Context manager to temporarily override an object's freezability.

    On entry, saves the object's current effective freezability (which may
    be inherited from the type) and applies the requested override.
    On exit, restores the saved status via set_freezable().

    Design note: on exit we always call set_freezable() with the value
    that get_freezable() returned on entry, even if that value was
    inherited from the type rather than set directly on the object.
    This means the context manager may leave a per-object status where
    none existed before.  We considered two alternatives:

      * "Best-effort unset": use a get_direct_freezable() that inspects
        only the object's own __dict__ / ob_flags, and call
        unset_freezable() when no direct status existed.  This mostly
        works, but C types with custom tp_getattro cannot reliably
        distinguish per-object from inherited status, so the "direct"
        check is incomplete for some extension types.

      * "No-effort unset" (chosen): always snapshot the effective status
        and restore it with set_freezable().  Simple, correct for all
        object kinds, and predictable.  The trade-off is that after the
        context manager exits, the object will have an explicit
        per-object status even if it previously relied on inheritance.

    Usage:
        with FreezabilityOverride(obj, FREEZABLE_NO):
            # obj is temporarily not freezable
            ...
        # obj's original effective freezability is restored
    """

    def __init__(self, obj, status):
        self._obj = obj
        self._new_status = status
        self._old_status = None

    def __enter__(self):
        self._old_status = get_freezable(self._obj)
        set_freezable(self._obj, self._new_status)
        return self._obj

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self._old_status == -1:
            # No status was set anywhere (not on object, not on type).
            # Restore to the default: FREEZABLE_YES.
            set_freezable(self._obj, FREEZABLE_YES)
        else:
            set_freezable(self._obj, self._old_status)
        return False


class require_mutable(FreezabilityOverride):
    """Context manager that ensures an object remains mutable (not freezable).

    Raises TypeError if the object is already frozen.

    Usage:
        with require_mutable(obj):
            obj.attr = value  # guaranteed not to be frozen during this block
    """

    def __init__(self, obj):
        super().__init__(obj, FREEZABLE_NO)

    def __enter__(self):
        if is_frozen(self._obj):
            raise TypeError(
                "cannot require mutability: object is already frozen")
        return super().__enter__()


__all__ = [
    "freeze",
    "is_frozen",
    "set_freezable",
    "NotFreezableError",
    "ImmutableModule",
    "FREEZABLE_YES",
    "FREEZABLE_NO",
    "FREEZABLE_EXPLICIT",
    "FREEZABLE_PROXY",
    "InterpreterLocal",
    "SharedField",
    "freezable",
    "unfreezable",
    "explicitlyFreezable",
    "frozen",
]

__version__ = getattr(_c, "__version__", "1.0")
