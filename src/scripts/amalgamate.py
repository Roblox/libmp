#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Generate the single-header/single-source LibMP distribution.

First-party wrappers opt vendored code into expansion with paired annotations:
  // @amalgamate: dependency-begin NAME CONFIG_DEFINE BUNDLED_ROOT
  #include ...
  // @amalgamate: dependency-end NAME

Bundled mode expands includes from BUNDLED_ROOT. In external mode, angle includes
remain as host-facing includes and quoted vendored-source includes are omitted.
Direct .c includes in an annotated region are its authoritative source entries;
recursive includes of those same files are omitted from the generated output.

Configuration-specific source regions use:
  // @amalgamate: config-begin LABEL [BOOLEAN_EXPRESSION]
  ...
  // @amalgamate: config-end LABEL

If BOOLEAN_EXPRESSION is omitted, LABEL is also the boolean define name.
Expressions support identifiers, 0, 1, !, &&, ||, and parentheses.

A configuration annotation must be directly inside exactly one preprocessor
branch. BOOLEAN_EXPRESSION controls whether the generator retains its body; it
may represent only the generation-time portion of the surrounding branch, whose
remaining terms stay in the output for the compiler to evaluate. A dependency
annotation may be at top level or directly inside its one bundled/external
selection branch. Additional enclosing conditionals are rejected so conditional
include deduplication stays sound.
"""

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple, Union

from _build_metadata import render_line_comment_header
from _utils import compiler_define_enabled, utc_timestamp


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
# Plain-text so CMake and Python can share it without another config parser.
SOURCE_DIRS_FILE = PROJECT_ROOT / "source-dirs.txt"

PUBLIC_HEADERS = (
    Path("mp_api/c_api.h"),
    Path("mp_api/cpp_api.h"),
)

SOURCE_DIRS = tuple(
    Path(line.strip())
    for line in SOURCE_DIRS_FILE.read_text(encoding="utf-8").splitlines()
    if line.strip()
)

INCLUDE_DIRECTIVE_RE = re.compile(r"^\s*#\s*include\b(?P<operand>.*)$")
LITERAL_INCLUDE_RE = re.compile(
    r'^\s*(?:"(?P<quoted>[^"]+)"|<(?P<angled>[^>]+)>)(?:\s*(?://.*|/\*.*\*/\s*)?)?$'
)
PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\s*(?://.*)?$")
CONDITIONAL_OPEN_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef)\b")
CONDITIONAL_CLOSE_RE = re.compile(r"^\s*#\s*endif\b")
IFNDEF_RE = re.compile(r"^\s*#\s*ifndef\s+(?P<name>[A-Za-z_]\w*)\s*(?://.*)?$")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+(?P<name>[A-Za-z_]\w*)(?:\s|$)")
DEPENDENCY_BEGIN_RE = re.compile(
    r"^\s*//\s*@amalgamate:\s*dependency-begin\s+"
    r"(?P<name>[a-z0-9_-]+)\s+(?P<define>[A-Za-z_]\w*)\s+(?P<root>\S+)\s*$"
)
DEPENDENCY_END_RE = re.compile(
    r"^\s*//\s*@amalgamate:\s*dependency-end\s+(?P<name>[a-z0-9_-]+)\s*$"
)
CONFIG_BEGIN_RE = re.compile(
    r"^\s*//\s*@amalgamate:\s*config-begin\s+"
    r"(?P<label>[A-Za-z_][\w-]*)(?:\s+(?P<expression>.+?))?\s*$"
)
CONFIG_END_RE = re.compile(
    r"^\s*//\s*@amalgamate:\s*config-end\s+(?P<label>[A-Za-z_][\w-]*)\s*$"
)
CONFIG_TOKEN_RE = re.compile(r"\s*(\|\||&&|!|\(|\)|[A-Za-z_]\w*|[01])")


@dataclass(frozen=True)
class DependencySpec:
    name: str
    define: str
    root: Path


@dataclass(frozen=True)
class ConfigurationSpec:
    label: str
    expression: str


@dataclass(frozen=True)
class SourceAnnotation:
    kind: str
    action: str
    label: str
    conditional_depth: int
    specification: Optional[Union[DependencySpec, ConfigurationSpec]] = None


@dataclass(frozen=True)
class ParsedLine:
    number: int
    text: str
    conditional_depth: int
    include: Optional[Tuple[str, bool]] = None
    resolved_include: Optional[Path] = None
    pragma_once: bool = False
    annotation: Optional[SourceAnnotation] = None


@dataclass(frozen=True)
class ParsedSource:
    path: Path
    relative: str
    lines: Tuple[ParsedLine, ...]
    guarded: bool


def relative_path(path):
    return path.resolve().relative_to(PROJECT_ROOT.resolve())


def is_under(path, root):
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def first_party_roots():
    return tuple(PROJECT_ROOT / directory for directory in SOURCE_DIRS)


def first_party_files():
    for root in first_party_roots():
        for path in sorted(root.rglob("*")):
            if path.suffix in {".c", ".cpp", ".h", ".inc"}:
                yield path


def configuration_tokens(expression):
    tokens = []
    position = 0
    while position < len(expression):
        match = CONFIG_TOKEN_RE.match(expression, position)
        if match is None:
            raise RuntimeError(f"invalid configuration expression: {expression}")
        tokens.append(match.group(1))
        position = match.end()
    return tokens


def configuration_defines(specs):
    """Return the boolean defines referenced by annotated expressions."""
    defines = set()
    for spec in specs.values():
        defines.update(
            token for token in configuration_tokens(spec.expression)
            if re.fullmatch(r"[A-Za-z_]\w*", token)
        )
    return defines


def boolean_define_values(definitions, names):
    """Resolve annotated boolean defines; an absent define has its normal value 0."""
    return {
        name: compiler_define_enabled(definitions, name)
        for name in names
    }


def evaluate_configuration(expression, values):
    """Evaluate the small boolean-expression language used by config annotations."""
    tokens = configuration_tokens(expression)
    position = 0

    def parse_primary():
        nonlocal position
        if position >= len(tokens):
            raise RuntimeError(f"incomplete configuration expression: {expression}")
        token = tokens[position]
        position += 1
        if token == "(":
            result = parse_or()
            if position >= len(tokens) or tokens[position] != ")":
                raise RuntimeError(f"unbalanced configuration expression: {expression}")
            position += 1
            return result
        if token == "0":
            return False
        if token == "1":
            return True
        if re.fullmatch(r"[A-Za-z_]\w*", token):
            return values[token]
        raise RuntimeError(f"unexpected {token!r} in configuration expression: {expression}")

    def parse_unary():
        nonlocal position
        if position < len(tokens) and tokens[position] == "!":
            position += 1
            return not parse_unary()
        return parse_primary()

    def parse_and():
        nonlocal position
        result = parse_unary()
        while position < len(tokens) and tokens[position] == "&&":
            position += 1
            rhs = parse_unary()
            result = result and rhs
        return result

    def parse_or():
        nonlocal position
        result = parse_and()
        while position < len(tokens) and tokens[position] == "||":
            position += 1
            rhs = parse_and()
            result = result or rhs
        return result

    result = parse_or()
    if position != len(tokens):
        raise RuntimeError(f"unexpected {tokens[position]!r} in configuration expression: {expression}")
    return result


def is_first_party_source(path):
    return any(is_under(path, root) for root in first_party_roots())


def is_first_party_input(path):
    return path.suffix in {".c", ".h", ".inc"} and is_first_party_source(path)


def resolve_local_include(including_file, name, quoted=True):
    candidates = [PROJECT_ROOT / name, PROJECT_ROOT / "dependencies" / name]
    if quoted:
        candidates.insert(0, including_file.parent / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def parse_include(line, including_file, line_number):
    """Return (path, is_quoted), or reject configuration-dependent includes."""
    directive = INCLUDE_DIRECTIVE_RE.match(line.rstrip("\r\n"))
    if directive is None:
        return None
    literal = LITERAL_INCLUDE_RE.match(directive.group("operand"))
    if literal is None:
        if is_first_party_source(including_file):
            relative = relative_path(including_file).as_posix()
            operand = directive.group("operand").strip()
            raise RuntimeError(
                f"non-literal #include is unsupported in {relative}:{line_number}: {operand}"
            )
        return None
    quoted = literal.group("quoted")
    return (quoted or literal.group("angled"), quoted is not None)


def include_guard(path):
    relative = str(relative_path(path)).upper()
    token = re.sub(r"[^A-Z0-9]", "_", relative)
    return f"LIBMP_AMALGAMATED_INCLUDED_{token}"


def conventional_guard_lines(lines):
    """Return a proven whole-file include guard, allowing an extern-C shell."""
    in_block_comment = False
    significant_lines = []
    for line_number, line in enumerate(lines, 1):
        text = line.strip()
        if in_block_comment:
            if "*/" in text:
                in_block_comment = False
            continue
        if not text or text.startswith("//"):
            continue
        if text.startswith("/*"):
            if "*/" not in text:
                in_block_comment = True
            continue
        significant_lines.append((line_number, line))

    def is_extern_c_shell(prefix, suffix):
        if len(prefix) != 3 or len(suffix) != 3:
            return False
        prefix_text = [line.strip() for _, line in prefix]
        suffix_text = [line.strip() for _, line in suffix]
        return (
            CONDITIONAL_OPEN_RE.match(prefix_text[0]) is not None
            and "__cplusplus" in prefix_text[0]
            and re.fullmatch(r'extern\s+"C"\s*\{', prefix_text[1]) is not None
            and CONDITIONAL_CLOSE_RE.match(prefix_text[2]) is not None
            and CONDITIONAL_OPEN_RE.match(suffix_text[0]) is not None
            and "__cplusplus" in suffix_text[0]
            and prefix_text[0] == suffix_text[0]
            and re.fullmatch(r"}\s*(?://.*|/\*.*\*/)?", suffix_text[1]) is not None
            and CONDITIONAL_CLOSE_RE.match(suffix_text[2]) is not None
        )

    for (open_line, open_text), (define_line, define_text) in zip(
        significant_lines, significant_lines[1:]
    ):
        guard = IFNDEF_RE.match(open_text.rstrip("\r\n"))
        definition = DEFINE_RE.match(define_text.rstrip("\r\n"))
        if (
            guard is None
            or definition is None
            or guard.group("name") != definition.group("name")
        ):
            continue
        depth = 0
        for line_number in range(open_line, len(lines) + 1):
            line = lines[line_number - 1]
            if CONDITIONAL_OPEN_RE.match(line):
                depth += 1
            elif CONDITIONAL_CLOSE_RE.match(line):
                depth -= 1
                if depth == 0:
                    prefix = [item for item in significant_lines if item[0] < open_line]
                    suffix = [item for item in significant_lines if item[0] > line_number]
                    if (not prefix and not suffix) or is_extern_c_shell(prefix, suffix):
                        return open_line, define_line, line_number
                    break
    return None


def parse_source(path, lines=None):
    """Parse and validate the source facts needed during amalgamation."""
    path = path.resolve()
    relative = relative_path(path).as_posix()
    if lines is None:
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    has_pragma_once = any(PRAGMA_ONCE_RE.match(line) for line in lines)
    guard_lines = (
        conventional_guard_lines(lines)
        if path.suffix == ".h" and not has_pragma_once
        else None
    )
    conditional_depth = 0
    active_annotation = None
    parsed_lines = []

    def fail(line_number, message):
        raise RuntimeError(f"{message} in {relative}:{line_number}")

    for line_number, line in enumerate(lines, 1):
        stripped = line.rstrip("\r\n")
        config_begin = CONFIG_BEGIN_RE.match(stripped)
        config_end = CONFIG_END_RE.match(stripped)
        dependency_begin = DEPENDENCY_BEGIN_RE.match(stripped)
        dependency_end = DEPENDENCY_END_RE.match(stripped)
        annotation = None

        if config_begin is not None:
            if active_annotation is not None:
                fail(line_number, "nested amalgamation annotation")
            if conditional_depth != 1:
                fail(
                    line_number,
                    "configuration annotation must be directly inside one conditional branch",
                )
            expression = config_begin.group("expression") or config_begin.group("label")
            configuration_tokens(expression)
            annotation = SourceAnnotation(
                kind="configuration",
                action="begin",
                label=config_begin.group("label"),
                conditional_depth=conditional_depth,
                specification=ConfigurationSpec(
                    config_begin.group("label"), expression
                ),
            )
            active_annotation = annotation
        elif dependency_begin is not None:
            if active_annotation is not None:
                fail(line_number, "nested amalgamation annotation")
            if conditional_depth > 1:
                fail(
                    line_number,
                    "dependency annotation has more than one enclosing conditional",
                )
            specification = DependencySpec(
                dependency_begin.group("name"),
                dependency_begin.group("define"),
                (PROJECT_ROOT / dependency_begin.group("root")).resolve(),
            )
            if not specification.root.is_dir():
                fail(
                    line_number,
                    f"dependency {specification.name} root does not exist: "
                    f"{specification.root}",
                )
            annotation = SourceAnnotation(
                kind="dependency",
                action="begin",
                label=dependency_begin.group("name"),
                conditional_depth=conditional_depth,
                specification=specification,
            )
            active_annotation = annotation
        elif config_end is not None:
            if (
                active_annotation is None
                or active_annotation.kind != "configuration"
                or active_annotation.label != config_end.group("label")
            ):
                fail(line_number, "unmatched configuration annotation")
            if conditional_depth != active_annotation.conditional_depth:
                fail(line_number, "configuration annotation crosses a conditional boundary")
            annotation = SourceAnnotation(
                kind="configuration",
                action="end",
                label=config_end.group("label"),
                conditional_depth=conditional_depth,
            )
            active_annotation = None
        elif dependency_end is not None:
            if (
                active_annotation is None
                or active_annotation.kind != "dependency"
                or active_annotation.label != dependency_end.group("name")
            ):
                fail(line_number, "unmatched dependency annotation")
            if conditional_depth != active_annotation.conditional_depth:
                fail(line_number, "dependency annotation crosses a conditional boundary")
            annotation = SourceAnnotation(
                kind="dependency",
                action="end",
                label=dependency_end.group("name"),
                conditional_depth=conditional_depth,
            )
            active_annotation = None

        closes_conditional = (
            CONDITIONAL_CLOSE_RE.match(line) is not None
            and (guard_lines is None or line_number != guard_lines[2])
        )
        if closes_conditional:
            conditional_depth -= 1
            if conditional_depth < 0:
                fail(line_number, "unmatched preprocessor conditional")

        include = parse_include(line, path, line_number)
        parsed_lines.append(ParsedLine(
            number=line_number,
            text=line,
            conditional_depth=conditional_depth,
            include=include,
            resolved_include=resolve_local_include(path, *include) if include else None,
            pragma_once=PRAGMA_ONCE_RE.match(line) is not None,
            annotation=annotation,
        ))

        if (
            CONDITIONAL_OPEN_RE.match(line) is not None
            and (guard_lines is None or line_number != guard_lines[0])
        ):
            conditional_depth += 1

    if active_annotation is not None:
        fail(len(lines), f"unterminated {active_annotation.kind} annotation")
    if conditional_depth != 0:
        fail(len(lines), "unbalanced preprocessor conditionals")
    return ParsedSource(
        path=path,
        relative=relative,
        lines=tuple(parsed_lines),
        guarded=has_pragma_once or guard_lines is not None,
    )


class SourceCatalog:
    """One parsed and validated view shared by discovery and expansion."""

    def __init__(self):
        self._sources = {}
        self.dependencies = {}
        self.dependency_source_entries = {}
        self.configurations = {}
        for path in first_party_files():
            self.parse(path)

    @staticmethod
    def _add_spec(collection, key, specification, kind):
        previous = collection.get(key)
        if previous is not None and previous != specification:
            raise RuntimeError(f"conflicting {kind} annotations for {key}")
        collection[key] = specification

    def _register_annotations(self, source):
        current_dependency = None
        current_dependency_depth = None
        for parsed_line in source.lines:
            annotation = parsed_line.annotation
            if annotation is not None and annotation.kind == "dependency":
                if annotation.action == "begin":
                    current_dependency = annotation.specification
                    current_dependency_depth = annotation.conditional_depth
                    self._add_spec(
                        self.dependencies,
                        annotation.label,
                        annotation.specification,
                        "dependency",
                    )
                else:
                    current_dependency = None
                    current_dependency_depth = None
                continue
            if annotation is not None and annotation.action == "begin":
                self._add_spec(
                    self.configurations,
                    annotation.label,
                    annotation.specification,
                    "configuration",
                )
            if (
                current_dependency is not None
                and parsed_line.conditional_depth == current_dependency_depth
                and parsed_line.resolved_include is not None
                and parsed_line.resolved_include.suffix == ".c"
                and is_under(parsed_line.resolved_include, current_dependency.root)
            ):
                self.dependency_source_entries.setdefault(
                    current_dependency.name, set()
                ).add(parsed_line.resolved_include)

    def parse(self, path):
        path = path.resolve()
        source = self._sources.get(path)
        if source is None:
            source = parse_source(path)
            self._sources[path] = source
            if is_first_party_source(path):
                self._register_annotations(source)
        return source


def expand_file(
    path,
    catalog,
    emitted,
    external_dependencies,
    configuration_regions,
    dependency=None,
    active=(),
    conditional=False,
):
    path = path.resolve()
    source = catalog.parse(path)
    relative = source.relative
    if path in active:
        return [f"// Recursive include already active: {relative}\n"]

    if source.guarded and path in emitted:
        return [f"// Already amalgamated: {relative}\n"]
    if source.guarded and not conditional:
        # An unconditional inclusion always defines the generated guard, so later
        # copies can be omitted. Conditional inclusions must retain their body:
        # an earlier branch may be inactive for the eventual compiler settings.
        emitted.add(path)
    output = [f"// BEGIN {relative}\n"]
    if source.guarded:
        guard = include_guard(path)
        output.extend([f"#ifndef {guard}\n", f"#define {guard}\n"])
    output.append(f'#line 1 "{relative}"\n')

    next_active = (*active, path)
    current_dependency = dependency
    dependency_conditional_depth = 0 if dependency is not None else None
    current_configuration = None
    configuration_conditional_depth = None
    for parsed_line in source.lines:
        line_number = parsed_line.number
        line = parsed_line.text
        annotation = parsed_line.annotation
        if parsed_line.pragma_once:
            # Keep source line accounting exact after the preceding #line directive.
            output.append("\n")
            continue
        if annotation is not None and annotation.kind == "configuration":
            if annotation.action == "begin":
                current_configuration = annotation.label
                configuration_conditional_depth = annotation.conditional_depth
                output.append(line)
                retained = configuration_regions[current_configuration]
                state = "retained" if retained else "omitted"
                output.append(
                    f"// Configuration body {state}: "
                    f"{current_configuration}={int(retained)}\n"
                )
            else:
                body_was_omitted = not configuration_regions[current_configuration]
                current_configuration = None
                configuration_conditional_depth = None
                output.append(line)
                if body_was_omitted:
                    output.append(f'#line {line_number + 1} "{relative}"\n')
            continue
        if (
            current_configuration is not None
            and not configuration_regions[current_configuration]
        ):
            continue
        if annotation is not None and annotation.kind == "dependency":
            if annotation.action == "end":
                current_dependency = None
                dependency_conditional_depth = None
                output.append(line)
                continue
            if current_dependency is not None:
                raise RuntimeError(
                    f"nested dependency annotation in {relative}:{line_number}"
                )
            current_dependency = annotation.specification
            dependency_conditional_depth = annotation.conditional_depth
            output.append(line)
            continue
        include = parsed_line.include
        resolved = parsed_line.resolved_include
        dependency_input = (
            resolved is not None
            and current_dependency is not None
            and current_dependency.name not in external_dependencies
            and is_under(resolved, current_dependency.root)
        )
        omitted_dependency_source = (
            resolved is not None
            and current_dependency is not None
            and current_dependency.name in external_dependencies
            and is_under(resolved, current_dependency.root)
            and include[1]
        )
        nested_dependency_source_entry = (
            dependency_input
            and dependency is not None
            and resolved in catalog.dependency_source_entries.get(
                current_dependency.name, ()
            )
        )
        if resolved is not None and (is_first_party_input(resolved) or dependency_input):
            if nested_dependency_source_entry:
                output.append(
                    "// Dependency source already supplied by its wrapper: "
                    f"{relative_path(resolved).as_posix()}\n"
                )
            else:
                local_conditional_depth = parsed_line.conditional_depth
                if configuration_conditional_depth is not None:
                    local_conditional_depth -= configuration_conditional_depth
                elif dependency_conditional_depth is not None:
                    local_conditional_depth -= dependency_conditional_depth
                output.extend(expand_file(
                    resolved,
                    catalog,
                    emitted,
                    external_dependencies,
                    configuration_regions,
                    current_dependency if dependency_input else None,
                    next_active,
                    conditional=conditional or local_conditional_depth > 0,
                ))
            output.append(f'#line {line_number + 1} "{relative}"\n')
        elif omitted_dependency_source:
            output.append(f"// External dependency source omitted: {include[0]}\n")
        else:
            output.append(line)

    if source.lines and not source.lines[-1].text.endswith("\n"):
        output.append("\n")
    if source.guarded:
        output.append(f"#endif // {include_guard(path)}\n")
    output.append(f"// END {relative}\n")
    return output


def implementation_sources():
    return tuple(sorted(
        path.relative_to(PROJECT_ROOT)
        for directory in SOURCE_DIRS
        for path in (PROJECT_ROOT / directory).glob("*.cpp")
    ))


def generated_file(
    kind,
    roots,
    catalog,
    external_dependencies,
    configuration_values,
    configuration_regions,
    generation_timestamp,
    emitted=None,
):
    specs = catalog.dependencies
    dependency_modes = ", ".join(
        f"{name}={'external' if name in external_dependencies else 'bundled'}"
        for name in sorted(specs)
    )
    configuration_modes = ", ".join(
        f"{name}={1 if enabled else 0}"
        for name, enabled in sorted(configuration_values.items())
    )
    bundled_dependencies = tuple(
        name for name in ("flatbuffers", "lz4")
        if name in specs and name not in external_dependencies
    )
    documented_external_dependencies = tuple(
        name for name in ("flatbuffers", "lz4")
        if name in specs and name in external_dependencies
    )
    banner = (
        render_line_comment_header(
            "//",
            build_timestamp=generation_timestamp,
            bundled_dependencies=bundled_dependencies,
            external_dependencies=documented_external_dependencies,
            local_header_lines=(
                "LibMP for C++",
                "Amalgamated library for accessing Roblox MicroProfiler data.",
                "%LIBMP_SHARED_METADATA%",
                "",
                "Requirements:",
                "- A C++17 compiler; compile and link libmp.cpp with the application.",
                "- Supply any dependency marked external above.",
                "",
                "Minimal usage:",
                '  #include "libmp.h"',
                "  LibMP::Init();",
                '  auto session = LibMP::Session::OpenFromFile("capture.x.html");'
                ' // or "capture.gprx"',
            ),
            generated_by="scripts/amalgamate.py",
        )
        + "\n// Regenerate from the canonical source tree.\n"
        f"// Dependencies: {dependency_modes}.\n"
        f"// Configuration: {configuration_modes}.\n\n"
    )
    output = [banner]
    if emitted is None:
        emitted = set()
    if kind == "header":
        output.append("#pragma once\n\n")
        for name, spec in sorted(specs.items()):
            mode = 1 if name in external_dependencies else 0
            output.extend([
                f"#ifndef {spec.define}\n",
                f"    #define {spec.define} {mode}\n",
                f"#elif {spec.define} != {mode}\n",
                f'    #error "libmp.h was generated for a different {name} dependency mode"\n',
                "#endif\n",
            ])
        for define, enabled in sorted(configuration_values.items()):
            mode = 1 if enabled else 0
            output.extend([
                f"#ifndef {define}\n",
                f"    #define {define} {mode}\n",
                f"#elif {define} != {mode}\n",
                f'    #error "libmp.h was generated for {define}={mode}"\n',
                "#endif\n",
            ])
        output.append("\n")
    else:
        # Compile the implementation against the same generated public header,
        # selecting the internal ntd flavor exactly as cpp_api_internal.h does.
        output.extend([
            "#ifndef LIBMP_TYPES_INTERNAL\n",
            "    #define LIBMP_TYPES_INTERNAL\n",
            "    #define LIBMP_AMALGAMATED_UNDEFINE_TYPES_INTERNAL\n",
            "#endif\n",
            '#include "libmp.h"\n',
            "#ifdef LIBMP_AMALGAMATED_UNDEFINE_TYPES_INTERNAL\n",
            "    #undef LIBMP_TYPES_INTERNAL\n",
            "    #undef LIBMP_AMALGAMATED_UNDEFINE_TYPES_INTERNAL\n",
            "#endif\n\n",
        ])
    for root in roots:
        output.extend(expand_file(
            PROJECT_ROOT / root,
            catalog,
            emitted,
            external_dependencies,
            configuration_regions,
        ))
        output.append("\n")
    return "".join(output)


def write_if_changed(path, contents):
    if path.is_file() and path.read_text(encoding="utf-8") == contents:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(contents)


def generate(
    output_dir,
    external_dependencies=(),
    defines=(),
    catalog=None,
    generation_timestamp=None,
):
    output_dir = Path(output_dir).resolve()
    if generation_timestamp is None:
        generation_timestamp = utc_timestamp()
    catalog = SourceCatalog() if catalog is None else catalog
    specs = catalog.dependencies
    config_specs = catalog.configurations
    configuration_values = boolean_define_values(
        defines, configuration_defines(config_specs)
    )
    configuration_regions = {
        label: evaluate_configuration(spec.expression, configuration_values)
        for label, spec in config_specs.items()
    }
    external_dependencies = frozenset(external_dependencies)
    unknown = external_dependencies.difference(specs)
    if unknown:
        raise RuntimeError(
            f"unknown external dependencies: {', '.join(sorted(unknown))}"
        )
    header = output_dir / "libmp.h"
    source = output_dir / "libmp.cpp"
    header_emitted = set()
    header_contents = generated_file(
        "header",
        PUBLIC_HEADERS,
        catalog,
        external_dependencies,
        configuration_values,
        configuration_regions,
        generation_timestamp,
        header_emitted,
    )
    source_contents = generated_file(
        "source",
        implementation_sources(),
        catalog,
        external_dependencies,
        configuration_values,
        configuration_regions,
        generation_timestamp,
        set(header_emitted),
    )
    write_if_changed(header, header_contents)
    write_if_changed(source, source_contents)
    return header, source


def main():
    parser = argparse.ArgumentParser(
        description="Generate amalgamated libmp.h and libmp.cpp files"
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=PROJECT_ROOT / "build" / "amalgamated",
        help="output directory (default: src/build/amalgamated)",
    )
    catalog = SourceCatalog()
    specs = catalog.dependencies
    parser.add_argument(
        "--external-dependency",
        action="append",
        default=[],
        choices=sorted(specs),
        metavar="NAME",
        help="omit a bundled dependency body; may be repeated",
    )
    parser.add_argument(
        "--define",
        action="append",
        default=[],
        metavar="NAME[=0|1]",
        help="set a boolean build definition used by annotated regions; may be repeated",
    )
    args = parser.parse_args()

    header, source = generate(
        args.output_dir,
        external_dependencies=args.external_dependency,
        defines=args.define,
        catalog=catalog,
    )
    print(f"Generated {header}")
    print(f"Generated {source}")


if __name__ == "__main__":
    main()
