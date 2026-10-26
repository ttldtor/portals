"""Exercise affected naming selection in isolated Git repositories with Clang."""

from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.ci.naming_selection import _dependencies, _scan_arguments, select_sources


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="naming selection ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.git("init", "-q")
        self.git("config", "user.name", "Selector tests")
        self.git("config", "user.email", "selector@example.invalid")
        self.write("include/leaf.hpp", "#define VALUE 1\n")
        self.write("include/middle.hpp", '#include "leaf.hpp"\n')
        self.write("src/one.cpp", '#include "middle.hpp"\nint one = VALUE;\n')
        self.write("src/two.cpp", "int two = 2;\n")
        self.write("CMakeLists.txt", "# configuration\n")
        self.git("add", ".")
        self.git("commit", "-qm", "base")
        self.base = self.git("rev-parse", "HEAD")
        self.compiler = shutil.which("clang++")
        self.sources = {self.root / "src/one.cpp", self.root / "src/two.cpp"}
        self.entries = [self.entry(source) for source in sorted(self.sources)]

    def git(self, *arguments):
        return subprocess.check_output(["git", *arguments], cwd=self.root, text=True).strip()

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def entry(self, source, *flags):
        return {"directory": str(self.root), "file": str(source), "arguments": [
            self.compiler or "clang++", "-Iinclude", *flags, "-c", str(source),
            "-o", str(self.root / "unused.o"), "-MD", "-MF", "unused.d",
        ]}

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "change")
        return self.git("rev-parse", "HEAD")

    def select(self, head=None):
        return select_sources(self.root, self.entries, self.sources, self.base,
                              head or self.git("rev-parse", "HEAD"), 2)

    def assert_full(self, result):
        self.assertEqual(result["mode"], "full", result)
        self.assertEqual(result["selected_sources"], self.sources)
        self.assertTrue(result["reason"])

    def require_clang(self):
        if not self.compiler:
            self.skipTest("Clang POSIX driver is unavailable")

    def test_transitive_header_and_no_build_outputs(self):
        self.require_clang()
        self.write("include/leaf.hpp", "#define VALUE 3\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], {self.root / "src/one.cpp"})
        self.assertEqual(result["changed_paths"], ["include/leaf.hpp"])
        self.assertEqual(result["scan_commands"], 2)
        self.assertFalse((self.root / "unused.o").exists())
        self.assertFalse((self.root / "unused.d").exists())

    def test_posix_command_string_preserves_quoted_paths(self):
        self.require_clang()
        for entry in self.entries:
            entry["command"] = shlex.join(entry.pop("arguments"))
        self.write("include/leaf.hpp", "#define VALUE 6\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], {self.root / "src/one.cpp"})

    def test_make_escapes_and_line_continuations(self):
        text = ("naming_selection: src/one.cpp " + "\\" + "\n"
                + r" include/space\ name$$\#.hpp include/back\\slash.hpp" + "\n")
        self.assertEqual(_dependencies(text, self.root), {
            self.root / "src/one.cpp", self.root / "include/space name$#.hpp",
            (self.root / "include/back\\slash.hpp").resolve(),
        })

    def test_scan_command_removes_outputs_and_preserves_preprocessing_flags(self):
        arguments = ["clang++-20", "--gcc-install-dir=/toolchain/gcc", "-std=c++23",
                     "-I", "include space", "-isystem", "system", "-DMODE=2",
                     "-c", "-S", "-E", "src/one.cpp", "-oobject.o", "-MD", "-MMD", "-MP",
                     "-MF", "output.d", "-MTtarget", "-MQ", "quoted", "-MJoutput.json"]
        self.assertEqual(_scan_arguments({"arguments": arguments}), [
            "clang++-20", "--gcc-install-dir=/toolchain/gcc", "-std=c++23",
            "-I", "include space", "-isystem", "system", "-DMODE=2", "src/one.cpp",
        ])
        for option in ("-include-pch", "-include-pth", "-fimplicit-modules", "-fimplicit-module-maps",
                       "-working-directory", "--driver-mode=cl", "-save-temps"):
            with self.subTest(option=option), self.assertRaises(ValueError):
                _scan_arguments({"arguments": ["clang++", option, "src/one.cpp"]})

    def test_conditional_and_command_variant_union(self):
        self.require_clang()
        self.write("src/one.cpp", '#ifdef ENABLE_LEAF\n#include "leaf.hpp"\n#endif\n')
        self.commit()
        self.base = self.git("rev-parse", "HEAD")
        self.entries.append(self.entry(self.root / "src/one.cpp", "-DENABLE_LEAF"))
        self.write("include/leaf.hpp", "#define VALUE 4\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], {self.root / "src/one.cpp"})
        self.assertEqual(result["scan_commands"], 3)
        self.entries.pop()
        self.assertEqual(self.select()["selected_sources"], set())

    def test_included_cpp_is_scanned_as_dependency(self):
        self.require_clang()
        self.write("src/two.cpp", '#include "one.cpp"\n')
        self.commit()
        self.base = self.git("rev-parse", "HEAD")
        self.write("src/one.cpp", '#include "middle.hpp"\nint one = VALUE + 1;\n')
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], self.sources)

    def test_space_dollar_hash_header_and_system_include(self):
        self.require_clang()
        self.write("include/space $ cash #.hpp", "#define CASH 1\n")
        self.write("src/one.cpp", '#include <space $ cash #.hpp>\n')
        self.commit()
        self.base = self.git("rev-parse", "HEAD")
        self.entries[0] = self.entry(self.root / "src/one.cpp", "-isystem", "include")
        self.write("include/space $ cash #.hpp", "#define CASH 2\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], {self.root / "src/one.cpp"})

    def test_added_deleted_and_config_changes_fall_back(self):
        for name, content in [("include/new.hpp", "// added\n"),
                              ("include/leaf.hpp", None),
                              ("CMakeLists.txt", "# changed\n")]:
            with self.subTest(name=name):
                if content is None:
                    (self.root / name).unlink()
                else:
                    self.write(name, content)
                self.assert_full(self.select(self.commit()))
                self.git("reset", "--hard", self.base)

    def test_dirty_tracked_empty_diff_missing_history_and_wrong_head(self):
        self.assert_full(self.select())
        self.write("include/leaf.hpp", "#define VALUE 4\n")
        head = self.commit()
        self.write("src/two.cpp", "int dirty;\n")
        self.assert_full(self.select(head))
        self.git("restore", "src/two.cpp")
        self.assert_full(self.select(self.base))
        self.base = "missing-history"
        self.assert_full(self.select(head))

    def test_missing_git_falls_back(self):
        with patch("tools.ci.naming_selection.subprocess.run", side_effect=FileNotFoundError("git unavailable")):
            result = select_sources(self.root, self.entries, self.sources, self.base, "HEAD", 2)
        self.assert_full(result)
        self.assertIn("git unavailable", result["reason"])

    def test_untracked_artifacts_do_not_disable_selection(self):
        self.require_clang()
        self.write("include/leaf.hpp", "#define VALUE 5\n")
        head = self.commit()
        self.write("build/generated.txt", "untracked\n")
        self.assertEqual(self.select(head)["mode"], "affected")

    def test_entire_pr_range_includes_changes_before_the_latest_commit(self):
        self.require_clang()
        self.write("src/one.cpp", '#include "middle.hpp"\nint one = VALUE + 1;\n')
        self.commit()
        self.write("src/two.cpp", "int two = 3;\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], self.sources)
        self.assertEqual(result["changed_paths"], ["src/one.cpp", "src/two.cpp"])

    def test_unused_existing_header_can_select_no_translation_units(self):
        self.require_clang()
        self.write("include/unused.hpp", "#define UNUSED_VALUE 1\n")
        self.base = self.commit()
        self.write("include/unused.hpp", "#define UNUSED_VALUE 2\n")
        result = self.select(self.commit())
        self.assertEqual(result["mode"], "affected", result)
        self.assertEqual(result["selected_sources"], set())

    def test_untracked_and_ignored_include_shadows_require_full_check(self):
        self.require_clang()
        self.write("include/leaf.hpp", "#define VALUE 5\n")
        head = self.commit()
        self.write("src/middle.hpp", "#define VALUE 6\n")
        self.assert_full(self.select(head))
        self.git("config", "core.excludesFile", str(self.root / "local-ignore"))
        self.write("local-ignore", "src/middle.hpp\n")
        self.assert_full(self.select(head))

    def test_revision_text_cannot_become_a_git_output_option(self):
        self.write("include/leaf.hpp", "#define VALUE 5\n")
        head = self.commit()
        self.base = "--output=" + str(self.root / "unexpected-output")
        self.assert_full(self.select(head))
        self.assertFalse(list(self.root.glob("unexpected-output*")))

    def test_unsupported_command_and_incomplete_database(self):
        self.write("include/leaf.hpp", "#define VALUE 4\n")
        head = self.commit()
        for arguments in (["clang-cl", "/c", "src/one.cpp"],
                          ["ccache", "clang++", "-c", "src/one.cpp"],
                          ["clang++", "@flags.rsp", "src/one.cpp"]):
            with self.subTest(arguments=arguments):
                self.entries[0]["arguments"] = arguments
                self.assert_full(self.select(head))
        self.entries = self.entries[1:]
        self.assert_full(self.select(head))

    def test_scanner_failure_malformed_and_missing_main(self):
        self.write("include/leaf.hpp", "#define VALUE 4\n")
        head = self.commit()
        real_run = subprocess.run
        for depfile, returncode in [("invalid", 0), ("naming_selection: include/leaf.hpp\n", 0),
                                    ("", 1)]:
            temporary_files = []

            def fake_run(argv, **kwargs):
                if argv[0] == "git":
                    return real_run(argv, **kwargs)
                output = Path(argv[argv.index("-MF") + 1])
                temporary_files.append(output)
                output.write_text(depfile, encoding="utf-8")
                return subprocess.CompletedProcess(argv, returncode, b"", b"fake scanner failure")

            with self.subTest(depfile=depfile, returncode=returncode):
                with patch("tools.ci.naming_selection.subprocess.run", side_effect=fake_run):
                    self.assert_full(self.select(head))
                self.assertEqual(len(temporary_files), 2)
                self.assertEqual(len(set(temporary_files)), 2)
                self.assertTrue(all(not path.parent.exists() for path in temporary_files))


if __name__ == "__main__":
    unittest.main()
