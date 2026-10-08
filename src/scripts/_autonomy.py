# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Shared metadata for LibMP's native autonomy checks."""

from dataclasses import dataclass

from _utils import enabled_flag_arguments


@dataclass(frozen=True)
class AutonomyGate:
    name: str
    option: str
    label: str
    help: str
    supported_systems: tuple = ("Darwin", "Linux", "Windows")

    @property
    def argparse_dest(self):
        return self.option[2:].replace("-", "_")

    def supports(self, system):
        return system in self.supported_systems


NO_MALLOC_GATE = AutonomyGate(
    name="no-malloc",
    option="--strict-no-malloc",
    label="no direct libc allocation APIs",
    help="Reject direct malloc/free-family references in every archive member",
)

NO_PREMAIN_ALLOC_GATE = AutonomyGate(
    name="no-premain-alloc",
    option="--strict-no-premain-alloc",
    label="no allocator activity before main",
    help="No allocator activity reachable from static initializers",
)

AUTONOMY_GATES = (
    NO_MALLOC_GATE,
    NO_PREMAIN_ALLOC_GATE,
)

NO_CXX_STDLIB_OPTION = "--strict-no-cxx-stdlib"
NO_CXX_STDLIB_DEST = NO_CXX_STDLIB_OPTION[2:].replace("-", "_")
STRICT_OPTION = "--strict"
ALLOCATOR_AUTONOMOUS_STRICT_OPTION = "--allocator-autonomous-strict"


def add_autonomy_arguments(parser):
    parser.add_argument(
        STRICT_OPTION,
        action="store_true",
        help="Run every allocator-autonomy gate; requires a custom allocator backend",
    )
    parser.add_argument(
        NO_CXX_STDLIB_OPTION,
        action="store_true",
        help="Reserved link/run check for the NTD_OWN_STDLIB full-libstdc++-autonomy configuration",
    )
    for gate in AUTONOMY_GATES:
        parser.add_argument(gate.option, action="store_true", help=gate.help)
    parser.add_argument(
        ALLOCATOR_AUTONOMOUS_STRICT_OPTION,
        action="store_true",
        help="Shortcut for '--allocator-autonomous --strict'",
    )


def enabled_autonomy_arguments(args):
    options = (
        STRICT_OPTION,
        NO_CXX_STDLIB_OPTION,
        *(gate.option for gate in AUTONOMY_GATES),
        ALLOCATOR_AUTONOMOUS_STRICT_OPTION,
    )
    return enabled_flag_arguments(args, *options)
