"""Internal affected-TU selector for check_naming.py; no persistent dependency cache.

The runner supplies eligible sources and a compile database, plus exact PR refs.
Failures return the original source set in full mode; this helper never runs builds.
Compiler depfiles live in a temporary directory and are removed on every exit.
"""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from time import perf_counter


_SOURCE_ROOTS = {"src", "include", "tests", "tools", "examples", "experiments", "fuzz"}
_CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tpp"}
_TARGET = "naming_selection"


def _git(root, *arguments):
    result = subprocess.run(["git", *arguments], cwd=root, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=True)
    return result.stdout.decode("utf-8", errors="strict")


def _source_path(entry, root):
    directory = Path(entry["directory"])
    if not directory.is_absolute():
        directory = root / directory
    source = Path(entry["file"])
    if not source.is_absolute():
        source = directory / source
    return directory.resolve(), source.resolve()


def _scan_arguments(entry):
    arguments = entry.get("arguments")
    if arguments is None:
        command = entry["command"]
        # A compilation database command is shell-escaped. Only a single plain
        # POSIX driver invocation is supported; never execute it through a shell.
        if any(character in command for character in "\n\r`$|;&<>"):
            raise ValueError("unsupported shell command construction")
        arguments = shlex.split(command, posix=True)
    if not isinstance(arguments, list) or not arguments or not all(isinstance(arg, str) for arg in arguments):
        raise ValueError("invalid compiler arguments")
    driver = Path(arguments[0]).name
    if not re.fullmatch(r"clang(?:\+\+)?(?:-[0-9]+)?(?:\.[eE][xX][eE])?", driver):
        raise ValueError(f"unsupported compiler driver: {driver}")
    cleaned = [arguments[0]]
    index = 1
    value_options = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
    switches = {"-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MP", "-MG"}
    while index < len(arguments):
        argument = arguments[index]
        if argument.startswith("@") or argument in {"&&", "||", ";", "|", ">", "<", "--"}:
            raise ValueError("unsupported response file or command construction")
        # These escape driver parsing or request additional compiler outputs.
        if argument.startswith(("-Wp,", "-Xclang", "-save-temps", "-ftime-trace", "-fmodules",
                                "-fmodule-", "-fprofile-", "-serialize-diagnostics",
                                "--serialize-diagnostics", "-include-pch", "-include-pth",
                                "-fimplicit-modules", "-fimplicit-module-maps",
                                "-working-directory", "--driver-mode=")):
            raise ValueError(f"unsupported compiler option: {argument}")
        if argument in value_options:
            if index + 1 >= len(arguments):
                raise ValueError(f"missing value for {argument}")
            index += 2
            continue
        if argument in switches or any(argument.startswith(option) and argument != option for option in value_options):
            index += 1
            continue
        cleaned.append(argument)
        index += 1
    return cleaned


def _dependencies(text, directory):
    """Read the single make rule emitted by Clang, including make path escapes."""
    prefix = _TARGET + ":"
    if not text.startswith(prefix):
        raise ValueError("dependency output has no expected target")
    body = text[len(prefix):].replace("\\\r\n", "").replace("\\\n", "")
    tokens = []
    token = []
    index = 0
    while index < len(body):
        character = body[index]
        if character.isspace():
            if token:
                tokens.append("".join(token))
                token = []
            if character in "\r\n" and body[index:].strip():
                raise ValueError("dependency output contains multiple rules")
        elif character == "\\":
            index += 1
            if index == len(body):
                raise ValueError("unterminated dependency escape")
            escaped = body[index]
            if escaped not in " \t#\\:":
                token.append("\\")
            token.append(escaped)
        elif character == "$":
            index += 1
            if index == len(body) or body[index] != "$":
                raise ValueError("unsupported make expansion")
            token.append("$")
        elif character == "#":
            raise ValueError("unescaped dependency comment")
        else:
            token.append(character)
        index += 1
    if token:
        tokens.append("".join(token))
    if not tokens:
        raise ValueError("empty dependency list")
    dependencies = set()
    for token in tokens:
        path = Path(token)
        dependencies.add((directory / path).resolve() if not path.is_absolute() else path.resolve())
    return dependencies


def _scan(command, directory, source, depfile):
    arguments = [*command, "-M", "-MF", str(depfile), "-MT", _TARGET]
    result = subprocess.run(arguments, cwd=directory, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        diagnostic = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(f"dependency scanner failed for {source}: {diagnostic}")
    dependencies = _dependencies(depfile.read_text(encoding="utf-8"), directory)
    if source not in dependencies:
        raise ValueError(f"dependency output omits main source: {source}")
    return source, dependencies


def select_sources(root: Path, entries: list, sources: set[Path], base: str,
                   head: str, jobs: int) -> dict:
    """Select all TUs depending on existing modified project C++ files.

    Called by check_naming.py only with both refs. Unsupported changes, commands,
    Git failures or incomplete dependency evidence conservatively select all TUs.
    scan_commands counts compiler invocations, including failed invocations.
    """
    started = perf_counter()
    changed_paths = []
    scan_commands = 0

    def report(mode, reason, selected):
        return {"mode": mode, "reason": reason, "changed_paths": changed_paths,
                "selected_sources": set(selected), "scan_seconds": perf_counter() - started,
                "scan_commands": scan_commands}

    try:
        root = root.resolve()
        current = _git(root, "rev-parse", "--verify", "HEAD^{commit}").strip()
        requested = _git(root, "rev-parse", "--verify", "--end-of-options", f"{head}^{{commit}}").strip()
        baseline = _git(root, "rev-parse", "--verify", "--end-of-options", f"{base}^{{commit}}").strip()
        if current != requested:
            return report("full", "requested head is not the checked-out HEAD", sources)
        if _git(root, "status", "--porcelain", "--untracked-files=no").strip():
            return report("full", "tracked checkout is dirty", sources)
        # Ignored headers can shadow a tracked include too. Restrict the query
        # to source roots so ordinary build directories need no expensive walk.
        untracked = _git(root, "ls-files", "--others", "-z", "--", *sorted(_SOURCE_ROOTS))
        for name in filter(None, untracked.split("\0")):
            path = Path(name)
            if "__pycache__" not in path.parts or path.suffix != ".pyc":
                return report("full", f"untracked source-root input: {name}", sources)
        diff = _git(root, "diff", "--name-status", "--no-renames", "-z", f"{baseline}...{requested}", "--")
        records = diff.split("\0")
        if records.pop() != "" or len(records) % 2:
            raise ValueError("malformed Git name-status output")
        changes = list(zip(records[::2], records[1::2]))
        changed_paths.extend(path for _, path in changes)
        if not changes:
            return report("full", "empty PR diff", sources)
        for status, name in changes:
            path = Path(name)
            target = root / path
            if (status != "M" or not path.parts or path.parts[0] not in _SOURCE_ROOTS
                    or path.suffix.lower() not in _CPP_SUFFIXES or target.is_symlink()
                    or not target.is_file() or not target.resolve().is_relative_to(root)):
                return report("full", f"unsupported change: {status} {name}", sources)
        changed = {(root / name).resolve() for name in changed_paths}
        commands = []
        covered = set()
        for entry in entries:
            directory, source = _source_path(entry, root)
            if source in sources:
                commands.append((_scan_arguments(entry), directory, source))
                covered.add(source)
        if covered != sources:
            return report("full", "compile database does not cover every eligible source", sources)
        selected = sources & changed
        with tempfile.TemporaryDirectory(prefix="naming-dependencies-") as temporary:
            # Scan every variant, even when multiple commands
            # name the same TU. A scan error invalidates the entire selection.
            with ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
                futures = []
                for index, (command, directory, source) in enumerate(commands):
                    futures.append(executor.submit(_scan, command, directory, source,
                                                   Path(temporary) / f"{index}.d"))
                    scan_commands += 1
                for future in futures:
                    source, dependencies = future.result()
                    if dependencies & changed:
                        selected.add(source)
        return report("affected", f"scanned {len(commands)} commands for {len(covered)} sources; "
                      f"selected {len(selected)} sources", selected)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        return report("full", str(error), sources)
