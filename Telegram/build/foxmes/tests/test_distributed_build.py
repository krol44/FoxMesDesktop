import io
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest


HELPER = Path(__file__).resolve().parents[1] / "distributed-build.py"


@unittest.skipIf(os.name == "nt", "The compiler proxy fixture runs on POSIX hosts.")
class DistributedBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake") or not shutil.which("ninja"):
            raise RuntimeError("CMake and Ninja are required for distributed build tests")
        cls.temporary = tempfile.TemporaryDirectory(prefix="foxmes distributed ")
        cls.root = Path(cls.temporary.name).resolve()
        cls.source = cls.root / "source"
        cls.source.mkdir()
        cls.build = cls.root / "objects"
        cls.archives = cls.root / "archives"
        cls.archives.mkdir()
        cls.calls = cls.root / "compiler-calls"
        cls.revision = subprocess.check_output(
            ["git", "-C", str(HELPER.parents[3]), "rev-parse", "HEAD"], text=True).strip()
        cls.configurations = 0
        cls.rcc = os.environ.get("FOXMES_TEST_RCC") or shutil.which("rcc")
        if cls.rcc and not Path(cls.rcc).is_file():
            raise RuntimeError("FOXMES_TEST_RCC must name an installed Qt resource compiler")
        cls.compiler = shutil.which("c++")
        if not cls.compiler:
            raise RuntimeError("A C++ compiler is required for distributed build tests")
        (cls.source / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.25)
project(Shards LANGUAGES CXX)
add_library(base STATIC base.cpp)
add_executable(generator generator.cpp)
target_link_libraries(generator PRIVATE base)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated.cpp COMMAND generator ${CMAKE_CURRENT_BINARY_DIR}/generated.cpp DEPENDS generator VERBATIM)
add_library(ui OBJECT ui.cpp)
target_precompile_headers(ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)
add_executable(Telegram main.cpp extra.cpp ${CMAKE_CURRENT_BINARY_DIR}/generated.cpp)
target_link_libraries(Telegram PRIVATE base ui)
""", encoding="utf-8")
        files = {
            "base.cpp": "int base() { return 4; }\n",
            "generator.cpp": '#include <fstream>\nint main(int argc, char **argv) { std::ofstream(argv[1]) << "int generated() { return 7; }\\n"; }\n',
            "pch.h": "#include <vector>\n",
            "ui.cpp": "int ui() { return int(std::vector<int>{1,2}.size()); }\n",
            "extra.cpp": "int extra() { return 3; }\n",
            "main.cpp": "int base(); int ui(); int extra(); int generated(); int main() { return base()+ui()+extra()+generated()==16 ? 0 : 1; }\n",
        }
        for name, value in files.items():
            (cls.source / name).write_text(value, encoding="utf-8")
        if cls.rcc:
            (cls.source / "resource.txt").write_text("Fixed resource data.\n")
            (cls.source / "resource.qrc").write_text(
                '<RCC><qresource prefix="/fixture"><file>resource.txt</file></qresource></RCC>\n')
            with (cls.source / "CMakeLists.txt").open("a") as stream:
                stream.write('add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/qrc_fixture.cpp '
                    'COMMAND "' + cls.rcc + '" --name fixture --output ${CMAKE_CURRENT_BINARY_DIR}/qrc_fixture.cpp '
                    '${CMAKE_CURRENT_SOURCE_DIR}/resource.qrc '
                    'DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resource.qrc ${CMAKE_CURRENT_SOURCE_DIR}/resource.txt VERBATIM)\n'
                    'add_custom_target(resource_data DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/qrc_fixture.cpp)\n'
                    'add_dependencies(Telegram resource_data)\n')
        cls.proxy = cls.root / "compiler-proxy"
        cls.proxy.write_text(
            "#!" + sys.executable + "\n"
            "import os, shlex, sys\n"
            "from pathlib import Path\n"
            "args=[]\n"
            "for arg in sys.argv[1:]:\n"
            "    args.extend(shlex.split(Path(arg[1:]).read_text()) if arg.startswith('@') else [arg])\n"
            "if '-c' in args:\n"
            "    Path(os.environ['COMPILER_CALLS']).open('a').write('compile\\n')\n"
            "os.execv(os.environ['REAL_COMPILER'], [os.environ['REAL_COMPILER'], *sys.argv[1:]])\n",
            encoding="utf-8")
        cls.proxy.chmod(0o755)
        cls.tools = cls.root / "tools"
        cls.tools.mkdir()
        ccache = cls.tools / "ccache"
        ccache.write_text("#!/bin/sh\nexit 99\n")
        ccache.chmod(0o755)
        cls.environment = dict(
            os.environ, FOXMES_COLD_BUILD="1", COMPILER_CALLS=str(cls.calls),
            FOXMES_CCACHE_DIR=str(cls.root / "unused-cache"),
            PATH=str(cls.tools) + os.pathsep + os.environ["PATH"],
            REAL_COMPILER=cls.compiler, PYTHONDONTWRITEBYTECODE="1")
        cls.prepared = cls.root / "prepared"
        cls.prepared.mkdir()
        cls.configure()
        result = cls.run_helper("prepare", "--archive", str(cls.prepared / "prepare.tar.gz"))
        if result.returncode:
            raise RuntimeError(result.stdout)
        shutil.copyfile(cls.prepared / "prepare.tar.gz", cls.archives / "prepare.tar.gz")
        for index in range(4):
            cls.configure()
            result = cls.run_helper("shard", "--index", str(index), "--archive", str(cls.archives / f"{index}.tar.gz"), "--prepared-dir", str(cls.prepared))
            if result.returncode:
                raise RuntimeError(result.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def configure(cls, *arguments):
        if cls.build.exists():
            shutil.rmtree(cls.build)
        launcher = sys.executable + ";" + str(HELPER) + ";compiler"
        subprocess.run([
            "cmake", "-S", str(cls.source), "-B", str(cls.build), "-G", "Ninja Multi-Config",
            "-DCMAKE_CONFIGURATION_TYPES=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DCMAKE_CXX_COMPILER=" + str(cls.proxy),
            "-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher,
            "-DCMAKE_CXX_FLAGS=" + ("-Xclang -fno-pch-timestamp" if sys.platform == "darwin" else ""),
            *arguments,
        ], env=cls.environment, check=True, stdout=subprocess.DEVNULL)
        for path in cls.source.glob("*"):
            path.touch()
        cls.configurations += 1
        if cls.rcc:
            timestamp = 1700000000 + cls.configurations * 100
            os.utime(cls.source / "resource.txt", (timestamp, timestamp))

    @classmethod
    def run_helper(cls, phase, *arguments):
        return subprocess.run([
            sys.executable, str(HELPER), phase, "--build-dir", str(cls.build),
            "--revision", cls.revision, "--count", "4", *arguments,
        ], env=cls.environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    @unittest.skipUnless(shutil.which("swiftc"), "Swift is not installed")
    def test_swift_module_is_imported_without_compiling(self):
        self.configure()
        source = self.root / "fixture.swift"
        source.write_text("public func fixtureValue() -> Int32 { return 7 }\n")
        mapping = self.build / "swift-output-map.json"
        output = self.build / "fixture.o"
        module = self.build / "fixture.swiftmodule"
        mapping.write_text(json.dumps({"": {"object": str(output)}}))
        command = [
            sys.executable, str(HELPER), "native", shutil.which("swiftc"),
            "-c", "-parse-as-library", "-module-name", "Fixture", "-wmo",
            "-output-file-map", str(mapping), "-emit-module", "-emit-module-path", str(module), str(source),
        ]
        environment = dict(self.environment, FOXMES_DISTRIBUTED_BUILD_ROOT=str(self.build), FOXMES_DISTRIBUTED_PHASE="prepare")
        subprocess.run(command, cwd=self.build, env=environment, check=True)
        self.assertTrue(output.is_file())
        self.assertTrue(module.is_file())
        before = output.read_bytes()
        environment["FOXMES_DISTRIBUTED_PHASE"] = "link"
        result = subprocess.run(command, cwd=self.build, env=environment, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Imported Swift object", result.stdout)
        self.assertEqual(output.read_bytes(), before)
        source.write_text("public func fixtureValue() -> Int32 { return 8 }\n")
        result = subprocess.run(command, cwd=self.build, env=environment, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("different source or compiler arguments", result.stderr)

    def test_qt_resource_timestamps_are_reproducible(self):
        if not self.rcc:
            self.skipTest("Qt rcc is not installed")
        generated = []
        for path in sorted(self.archives.glob("*.tar.gz")):
            with tarfile.open(path, "r:gz") as archive:
                try:
                    generated.append(archive.extractfile("files/qrc_fixture.cpp").read())
                except KeyError:
                    pass
        self.assertGreaterEqual(len(generated), 2)
        self.assertTrue(all(value == generated[0] for value in generated))

    def test_cold_build_does_not_use_compiler_cache(self):
        self.assertFalse((self.root / "unused-cache").exists())

    def test_shared_objects_are_not_exported_by_individual_shards(self):
        with tarfile.open(self.prepared / "prepare.tar.gz", "r:gz") as archive:
            shared = set(json.load(archive.extractfile("manifest.json"))["records"])
        self.assertTrue(shared)
        groups = []
        for index in range(4):
            with tarfile.open(self.archives / f"{index}.tar.gz", "r:gz") as archive:
                objects = set(json.load(archive.extractfile("manifest.json"))["records"])
            self.assertFalse(shared & objects)
            for previous in groups:
                self.assertFalse(previous & objects)
            groups.append(objects)

    def test_link_imports_objects_without_compiling(self):
        self.configure()
        before = self.calls.read_text()
        result = self.run_helper("link", "--objects-dir", str(self.archives))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.calls.read_text(), before)
        self.assertIn("Native source compilations: 0", result.stdout)
        subprocess.run([str(self.build / "Release" / "Telegram")], check=True)

    def test_compiler_response_file_is_imported(self):
        self.configure()
        entries = json.loads((self.build / "compile_commands.json").read_text())
        entry = next(item for item in entries if item["file"].endswith("/base.cpp"))
        arguments = shlex.split(entry["command"])
        output = self.build / entry["output"]
        output.parent.mkdir(parents=True, exist_ok=True)
        response = self.build / "compiler.rsp"
        response.write_text(" ".join(shlex.quote(value) for value in arguments[1:]))
        environment = dict(self.environment, FOXMES_DISTRIBUTED_BUILD_ROOT=str(self.build), FOXMES_DISTRIBUTED_PHASE="shard")
        command = [sys.executable, str(HELPER), "compiler", arguments[0], "@" + str(response)]
        subprocess.run(command, cwd=self.build, env=environment, check=True)
        before = self.calls.read_text()
        environment["FOXMES_DISTRIBUTED_PHASE"] = "link"
        subprocess.run(command, cwd=self.build, env=environment, check=True)
        self.assertEqual(self.calls.read_text(), before)

    def test_changed_source_is_rejected(self):
        path = self.source / "main.cpp"
        original = path.read_text()
        try:
            path.write_text(original + "\nint changed_source;\n")
            self.configure()
            result = self.run_helper("link", "--objects-dir", str(self.archives))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("different source", result.stdout)
        finally:
            path.write_text(original)

    def test_changed_compile_flags_are_rejected(self):
        self.configure("-DCMAKE_CXX_FLAGS=-DCHANGED_BUILD")
        result = self.run_helper("link", "--objects-dir", str(self.archives))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build plan or paths do not match", result.stdout)

    def test_missing_shard_is_rejected(self):
        self.configure()
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            shutil.copyfile(self.prepared / "prepare.tar.gz", Path(directory) / "prepare.tar.gz")
            for index in range(3):
                shutil.copyfile(self.archives / f"{index}.tar.gz", Path(directory) / f"{index}.tar.gz")
            result = self.run_helper("link", "--objects-dir", directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Expected 5 object archives, found 4", result.stdout)

    def test_corrupt_object_archive_is_rejected(self):
        self.configure()
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            shutil.copyfile(self.prepared / "prepare.tar.gz", Path(directory) / "prepare.tar.gz")
            for index in range(1, 4):
                shutil.copyfile(self.archives / f"{index}.tar.gz", Path(directory) / f"{index}.tar.gz")
            changed = False
            with tarfile.open(self.archives / "0.tar.gz", "r:gz") as source:
                with tarfile.open(Path(directory) / "0.tar.gz", "w:gz") as target:
                    for member in source:
                        contents = source.extractfile(member).read()
                        if not changed and member.name.endswith(".o"):
                            contents = b"damaged object"
                            member.size = len(contents)
                            changed = True
                        target.addfile(member, io.BytesIO(contents))
            self.assertTrue(changed)
            result = self.run_helper("link", "--objects-dir", directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checksum mismatch", result.stdout)


if __name__ == "__main__":
    unittest.main()
