import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile


def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + str(os.getpid()) + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    os.replace(temporary, path)


def split_command(command):
    if os.name != "nt":
        return shlex.split(command)
    count = ctypes.c_int()
    function = ctypes.windll.shell32.CommandLineToArgvW
    function.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    function.restype = ctypes.POINTER(ctypes.c_wchar_p)
    arguments = function("placeholder " + command, ctypes.byref(count))
    if not arguments:
        raise RuntimeError("Cannot parse Windows compiler arguments")
    try:
        return [arguments[index] for index in range(1, count.value)]
    finally:
        release = ctypes.windll.kernel32.LocalFree
        release.argtypes = [ctypes.c_void_p]
        release.restype = ctypes.c_void_p
        release(arguments)


def expand_arguments(arguments):
    result = []
    for argument in arguments:
        if argument.startswith("@"):
            contents = Path(argument[1:]).read_bytes()
            encoding = "utf-16" if contents.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
            text = contents.decode(encoding)
            result.extend(expand_arguments(split_command(text)))
        else:
            result.append(argument)
    return result


def compiler_paths(arguments):
    output = source = None
    for index, argument in enumerate(arguments):
        if argument == "-o":
            output = arguments[index + 1]
        elif argument.startswith("/Fo"):
            output = argument[3:]
        elif argument in ("-c", "/c"):
            source = arguments[index + 1]
    if not output or not source:
        raise RuntimeError("Compiler invocation has no explicit source or output: " + repr(arguments))
    return Path(source).resolve(), Path(output).resolve()


def relative_output(path, build):
    try:
        return path.relative_to(build).as_posix()
    except ValueError as error:
        raise RuntimeError("Compiler output is outside the build directory: " + str(path)) from error


def command_digest(arguments):
    value = json.dumps([str(Path.cwd().resolve()), arguments], ensure_ascii=False)
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def swift_arguments(arguments):
    for index, value in enumerate(arguments):
        if Path(value).name.lower() in ("swiftc", "swiftc.exe"):
            return arguments[index:]
    return None


def swift_compiler(arguments):
    build = Path(os.environ["FOXMES_DISTRIBUTED_BUILD_ROOT"]).resolve()
    expanded = expand_arguments(arguments)
    mapping_path = Path(expanded[expanded.index("-output-file-map") + 1])
    mapping = read_json(mapping_path)
    sources = {str(Path(value).resolve()): digest(value) for value in expanded if value.endswith(".swift")}
    if not sources:
        raise RuntimeError("Swift invocation has no explicit source files")
    outputs = set()
    for entries in mapping.values():
        for value in entries.values():
            outputs.add(Path(value).resolve())
    module = Path(expanded[expanded.index("-emit-module-path") + 1]).resolve()
    outputs.update((module, module.with_suffix(".swiftdoc"), module.with_suffix(".swiftsourceinfo")))
    objects = sorted(path for path in outputs if path.suffix == ".o")
    if not objects:
        raise RuntimeError("Swift output map contains no objects")
    expected = {
        "source": next(iter(sources)), "source_digest": next(iter(sources.values())),
        "swift_sources": sources, "command_digest": command_digest(expanded),
    }
    records = []
    for output in objects:
        relative = relative_output(output, build)
        identifier = hashlib.sha256(relative.encode("utf-8")).hexdigest()
        path = build / "foxmes-distributed" / "records" / (identifier + ".json")
        records.append((relative, output, path, read_json(path) if path.is_file() else None))
    imported = os.environ.get("FOXMES_DISTRIBUTED_PHASE") == "link" or any(record and record.get("imported") for _, _, _, record in records)
    if imported:
        for relative, output, path, record in records:
            if not record or any(record.get(key) != value for key, value in expected.items()):
                raise RuntimeError("Imported Swift object has different source or compiler arguments: " + relative)
            for name, checksum in record["swift_outputs"].items():
                target = build / name
                if not target.is_file() or digest(target) != checksum:
                    raise RuntimeError("Imported Swift output is missing or damaged: " + name)
            print("Imported Swift object: " + relative, flush=True)
        return 0
    result = subprocess.run(arguments, check=False)
    if result.returncode:
        return result.returncode
    checksums = {
        relative_output(path, build): digest(path)
        for path in outputs
        if path.is_file() and path.suffix in (".o", ".swiftmodule", ".swiftdoc", ".swiftsourceinfo")
    }
    for relative, output, path, record in records:
        write_json(path, dict(expected, output=relative, output_digest=digest(output), swift_outputs=checksums))
    return 0


def native_command(arguments):
    if swift_arguments(arguments) == arguments:
        if "-c" in arguments or "-output-file-map" in arguments:
            return swift_compiler(arguments)
        if os.environ.get("FOXMES_DISTRIBUTED_PHASE") == "link" and "-emit-library" not in arguments:
            raise RuntimeError("Link-only build refuses an unrecognized Swift command")
    return subprocess.run(arguments, check=False).returncode


def compiler(arguments):
    build = Path(os.environ["FOXMES_DISTRIBUTED_BUILD_ROOT"]).resolve()
    expanded = expand_arguments(arguments)
    source, output = compiler_paths(expanded[1:])
    relative = relative_output(output, build)
    identifier = hashlib.sha256(relative.encode("utf-8")).hexdigest()
    record_path = build / "foxmes-distributed" / "records" / (identifier + ".json")
    expected = {
        "output": relative,
        "source": str(source),
        "source_digest": digest(source),
        "command_digest": command_digest(expanded),
    }
    imported = read_json(record_path) if record_path.is_file() else None
    if os.environ.get("FOXMES_DISTRIBUTED_PHASE") == "link" or (imported and imported.get("imported")):
        if not record_path.is_file() or not output.is_file():
            raise RuntimeError("Link-only build is missing imported object: " + relative)
        record = imported
        if any(record.get(key) != value for key, value in expected.items()):
            raise RuntimeError("Imported object has different source or compiler arguments: " + relative)
        if digest(output) != record["output_digest"]:
            raise RuntimeError("Imported object is damaged: " + relative)
        print("Imported object: " + relative, flush=True)
        return 0
    command = arguments
    if os.environ.get("FOXMES_CCACHE_DIR") and os.environ.get("FOXMES_COLD_BUILD") != "1":
        command = [shutil.which("ccache") or "ccache", *arguments]
    environment = os.environ.copy()
    if environment.get("FOXMES_COLD_BUILD") == "1":
        environment["CCACHE_DISABLE"] = "1"
    if environment.get("FOXMES_CCACHE_DIR"):
        environment["CCACHE_DIR"] = environment["FOXMES_CCACHE_DIR"]
    result = subprocess.run(command, env=environment, check=False)
    if result.returncode:
        return result.returncode
    expected["output_digest"] = digest(output)
    write_json(record_path, expected)
    return 0


def plan(build, revision):
    entries = read_json(build / "compile_commands.json")
    objects = {}
    compilers = {}
    for entry in entries:
        output = entry.get("output")
        arguments = split_command(entry["command"])
        if not output:
            _, output = compiler_paths(arguments[1:])
        output = Path(entry["directory"]) / output
        if output.suffix.lower() not in (".o", ".obj", ".pch", ".gch"):
            continue
        relative = relative_output(output.resolve(), build)
        objects[relative] = entry
        compiler_arguments = swift_arguments(arguments) if entry["file"].endswith(".swift") else arguments
        executable = Path(shutil.which(compiler_arguments[0]) or compiler_arguments[0]).resolve()
        if str(executable) not in compilers:
            compilers[str(executable)] = digest(executable)
    if not objects:
        raise RuntimeError("The compile database contains no object files")
    value = {
        "revision": revision,
        "source_date_epoch": os.environ.get("SOURCE_DATE_EPOCH", ""),
        "commands": objects,
        "compilers": compilers,
        "dependencies": os.environ.get("FOXMES_DEPENDENCY_CONTEXT", ""),
        "environment": {key: os.environ.get(key, "") for key in ("INCLUDE", "LIB", "CL", "_CL_", "SDKROOT", "DEVELOPER_DIR")},
        "configured_sources": {
            path.relative_to(build).as_posix(): digest(path)
            for path in build.rglob("*")
            if path.is_file()
            and not any(part.startswith("CompilerId") or part == "CMakeScratch" for part in path.parts)
            and path.suffix.lower() in (".h", ".hpp", ".hxx", ".inc", ".c", ".cpp", ".cxx", ".mm", ".m", ".swift")
        },
    }
    fingerprint = hashlib.sha256(json.dumps(value, sort_keys=True).encode("utf-8")).hexdigest()
    ordered = sorted(objects)
    ninja = "ninja"
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_MAKE_PROGRAM:FILEPATH="):
            ninja = line.split("=", 1)[1]
            break
    database = subprocess.run(
        [ninja, "-C", str(build), "-f", "build-Release.ninja", "-t", "compdb"],
        capture_output=True, text=True, encoding="utf-8", check=True)
    native_commands = {}
    for entry in json.loads(database.stdout):
        try:
            output = (Path(entry["directory"]) / entry["output"]).resolve().relative_to(build).as_posix()
        except ValueError:
            continue
        if output in objects:
            native_commands.setdefault(entry["command"], set()).add(output)
        elif entry.get("file", "").endswith(".swift"):
            native = swift_arguments(split_command(entry["command"]))
            for name, candidate in objects.items():
                if candidate["file"] == entry["file"] and swift_arguments(split_command(candidate["command"])) == native:
                    native_commands.setdefault(entry["command"], set()).add(name)
    closures = []
    for index in range(4):
        selected = ordered[index::4]
        if not selected:
            closures.append(set())
            continue
        outputs = set()
        for offset in range(0, len(selected), 30):
            result = subprocess.run(
                [ninja, "-C", str(build), "-f", "build-Release.ninja", "-t", "commands", *selected[offset:offset + 30]],
                capture_output=True, text=True, encoding="utf-8", check=True)
            for command in result.stdout.splitlines():
                outputs.update(native_commands.get(command, set()))
        closures.append(outputs)
    if set.union(*closures) != set(objects):
        raise RuntimeError("The Ninja compiler graph does not match the compile database")
    common = {name for name in objects if sum(name in closure for closure in closures) > 1 or objects[name]["file"].endswith(".swift")}
    print(f"Object plan: {len(ordered)} total, {len(common)} shared preparation objects", flush=True)
    return {"fingerprint": fingerprint, "revision": revision, "objects": ordered, "common_objects": sorted(common)}


def inventory(build):
    return {path.relative_to(build).as_posix() for path in build.rglob("*") if path.is_file()}


def run_native(build, targets, jobs):
    command = ["cmake", "--build", str(build), "--config", "Release", "--parallel", str(jobs)]
    subprocess.run([*command, "--target", *targets], check=True)


def export_shard(build, seed, configuration, index, count, archive):
    records = {}
    for path in (build / "foxmes-distributed" / "records").glob("*.json"):
        record = read_json(path)
        output = build / record["output"]
        if output.is_file() and not record.get("imported"):
            if digest(output) != record["output_digest"]:
                raise RuntimeError("Object changed after compilation: " + record["output"])
            records[record["output"]] = record
    files = (inventory(build) - seed) | set(records)
    files = {
        name for name in files
        if not name.startswith("foxmes-distributed/")
        and not Path(name).name.startswith(".ninja")
        and Path(name).name not in ("CMakeCache.txt", "compile_commands.json")
        and not name.endswith(".ninja")
    }
    manifest = {
        "configuration": configuration,
        "build_directory": str(build),
        "index": index,
        "count": count,
        "records": records,
        "files": {name: digest(build / name) for name in sorted(files)},
    }
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "manifest.json"
        write_json(path, manifest)
        with tarfile.open(archive, "w:gz", compresslevel=1, dereference=True) as bundle:
            bundle.add(path, arcname="manifest.json")
            for name in sorted(files):
                bundle.add(build / name, arcname="files/" + name, recursive=False)
    print(f"Exported shard {index + 1}/{count}: {len(records)} objects, {len(files)} files", flush=True)


def import_shards(build, configuration, directory, count, prepared_only=False):
    archives = sorted(directory.glob("*.tar.gz"))
    allowed = {-1} if prepared_only else {-1, *range(count)}
    if len(archives) != len(allowed):
        raise RuntimeError(f"Expected {len(allowed)} object archives, found {len(archives)}")
    indices = set()
    records = {}
    generated = {}
    for archive in archives:
        with tarfile.open(archive, "r:gz") as bundle:
            manifest_file = bundle.extractfile("manifest.json")
            if manifest_file is None:
                raise RuntimeError("Missing object archive manifest")
            manifest = json.load(manifest_file)
            if manifest["configuration"] != configuration or manifest["build_directory"] != str(build):
                raise RuntimeError("Object archive source, compiler, build plan or paths do not match: " + str(archive))
            index = manifest["index"]
            if manifest["count"] != count or index not in allowed or index in indices:
                raise RuntimeError("Duplicate or incompatible object shard: " + str(archive))
            indices.add(index)
            for name, expected_digest in manifest["files"].items():
                relative = PurePosixPath(name)
                if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
                    raise RuntimeError("Invalid object archive path: " + name)
                member = bundle.getmember("files/" + name)
                if not member.isfile():
                    raise RuntimeError("Object archive entry is not a regular file: " + name)
                target = build / name
                relative_output(target.resolve(), build)
                if name in generated:
                    if generated[name] != expected_digest and target.suffix.lower() in {
                        ".h", ".hpp", ".hxx", ".c", ".cpp", ".cc", ".cxx", ".m", ".mm", ".inc", ".swift",
                    }:
                        raise RuntimeError("Generated source differs between shards: " + name)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                stream = bundle.extractfile(member)
                if stream is None:
                    raise RuntimeError("Cannot read object archive entry: " + name)
                with target.open("wb") as output:
                    shutil.copyfileobj(stream, output)
                target.chmod(member.mode)
                if digest(target) != expected_digest:
                    raise RuntimeError("Object archive checksum mismatch: " + name)
                generated[name] = expected_digest
            for name, record in manifest["records"].items():
                if name in records:
                    comparable = ("output", "source", "source_digest", "command_digest")
                    if any(records[name][key] != record[key] for key in comparable):
                        raise RuntimeError("Object inputs differ between shards: " + name)
                    continue
                if generated.get(name) != record["output_digest"]:
                    raise RuntimeError("Imported object has no matching payload: " + name)
                records[name] = record
    required = configuration["common_objects"] if prepared_only else configuration["objects"]
    missing = set(required) - records.keys()
    if missing:
        raise RuntimeError("Shards did not compile required objects: " + ", ".join(sorted(missing)[:20]))
    for name, record in records.items():
        if not Path(record["source"]).is_file() or digest(record["source"]) != record["source_digest"]:
            raise RuntimeError("Imported object has a different source: " + name)
        for source, checksum in record.get("swift_sources", {}).items():
            if not Path(source).is_file() or digest(source) != checksum:
                raise RuntimeError("Imported Swift source is different: " + source)
        record["imported"] = True
        identifier = hashlib.sha256(name.encode("utf-8")).hexdigest()
        write_json(build / "foxmes-distributed" / "records" / (identifier + ".json"), record)
    print(f"Imported {len(records)} verified objects from {len(archives)} archives", flush=True)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "compiler":
        return compiler(sys.argv[2:])
    if len(sys.argv) > 1 and sys.argv[1] == "native":
        return native_command(sys.argv[2:])
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "shard", "link"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--index", type=int, default=0)
    parser.add_argument("--count", type=int, default=4)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--objects-dir", type=Path)
    parser.add_argument("--prepared-dir", type=Path)
    args = parser.parse_args()
    if args.count != 4 or args.index not in range(args.count) or args.jobs < 1:
        parser.error("Invalid shard count, shard index or parallel task count")
    build = args.build_dir.resolve()
    timestamp = subprocess.run(
        ["git", "-C", str(Path(__file__).resolve().parents[3]), "show", "-s", "--format=%ct", args.revision + "^{commit}"],
        capture_output=True, text=True, check=True).stdout.strip()
    if not timestamp.isdecimal() or int(timestamp) <= 0:
        raise RuntimeError("The source revision has no valid resource timestamp")
    os.environ["SOURCE_DATE_EPOCH"] = timestamp
    print("Resource timestamp: SOURCE_DATE_EPOCH=" + timestamp, flush=True)
    configuration = plan(build, args.revision)
    if os.environ.get("FOXMES_COLD_BUILD") == "1":
        print("Cold build: compiler cache is disabled; object artifacts belong to this run.", flush=True)
    os.environ["FOXMES_DISTRIBUTED_BUILD_ROOT"] = str(build)
    os.environ["FOXMES_DISTRIBUTED_PHASE"] = args.phase
    if args.phase in ("prepare", "shard"):
        if not args.archive:
            parser.error("--archive is required for preparation and shards")
        if args.phase == "prepare":
            assigned = configuration["common_objects"]
            index = -1
        else:
            if not args.prepared_dir:
                parser.error("--prepared-dir is required for a shard")
            import_shards(build, configuration, args.prepared_dir, args.count, prepared_only=True)
            common = set(configuration["common_objects"])
            assigned = [name for name in configuration["objects"][args.index::args.count] if name not in common]
            index = args.index
        seed = inventory(build)
        print(f"Compile phase {args.phase}, group {index}: {len(assigned)} assigned object files", flush=True)
        for offset in range(0, len(assigned), 30):
            run_native(build, assigned[offset:offset + 30], args.jobs)
        export_shard(build, seed, configuration, index, args.count, args.archive)
    else:
        if not args.objects_dir:
            parser.error("--objects-dir is required for linking")
        import_shards(build, configuration, args.objects_dir, args.count)
        run_native(build, ["Telegram"], args.jobs)
        print("Link-only build complete. Native source compilations: 0", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print("Distributed build failed:", error, file=sys.stderr, flush=True)
        raise SystemExit(1)
