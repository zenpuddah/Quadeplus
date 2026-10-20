#!/usr/bin/env python3
"""Exercise the exported native helper from an unrelated caller directory."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lexongine", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="external-host.", dir=args.scratch) as name:
        source = Path(name).resolve()
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(ExternalNativeHost LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(LEXONGINE_XML_FRONTEND OFF CACHE BOOL "")
set(LEXONGINE_QAEDALUS_BRIDGE OFF CACHE BOOL "")
add_subdirectory("{args.lexongine.resolve()}" "${{CMAKE_BINARY_DIR}}/engine")
add_executable(external_host probe.cpp)
lexongine_bind_host_build(external_host ROOT "${{CMAKE_CURRENT_SOURCE_DIR}}"
 INPUTS "${{CMAKE_CURRENT_SOURCE_DIR}}/probe.cpp" DEPENDENCY_FACTS "fixture-dependency-v1" BUILD_FACTS "external-caller-v1")
get_target_property(FINGERPRINT external_host LEXONGINE_HOST_SOURCE_FINGERPRINT)
file(WRITE "${{CMAKE_BINARY_DIR}}/fingerprint.txt" "${{FINGERPRINT}}")
''')
        probe = """#include "lexongine/plugins.hpp"
#include "lexongine/build_contract.hpp"
#include <iostream>
int main() {
 auto resources=lexongine::ResourceAccount::create({{16777216,4194304,536870912,33554432,4194304,67108864,8388608},4096,1024,8192,128});
 if(!std::holds_alternative<std::shared_ptr<lexongine::ResourceAccount>>(resources)) return 1;
 auto build=lexongine::inspect_host_build(lexongine::build_contract_id,LEXONGINE_HOST_SOURCE_BUILD,std::get<std::shared_ptr<lexongine::ResourceAccount>>(resources),{134217728,1048576,512});
 if(auto* error=std::get_if<lexongine::Diagnostic>(&build)) {std::cerr<<error->text();return 2;}
 std::cout << "SOURCE_REVISION";
}
"""
        fingerprints = []
        for revision in (1, 2):
            (source / "probe.cpp").write_text(probe.replace("SOURCE_REVISION", str(revision)))
            run("cmake", "-S", str(source), "-B", str(source / "build"), "-DCMAKE_BUILD_TYPE=Debug")
            fingerprints.append((source / "build/fingerprint.txt").read_text())
            run("cmake", "--build", str(source / "build"), "--target", "external_host", "-j", "4")
            output = run(str(source / "build/external_host"))
            if output != str(revision):
                raise RuntimeError("external native host did not execute the changed source")
        if len(fingerprints[0]) != 64 or fingerprints[0] == fingerprints[1]:
            raise RuntimeError("changed external host source did not change build identity")
        # Runtime validation must continue to reject modified sidecars.
        sidecar = source / "build/external_host.lexongine-build"
        sidecar.write_text(sidecar.read_text().replace(fingerprints[1], "0" * 64))
        invalid = subprocess.run([str(source / "build/external_host")], capture_output=True)
        if invalid.returncode == 0:
            raise RuntimeError("native host accepted a tampered source-build sidecar")
        print("EXTERNAL_HOST_BUILD_PASS source_change=distinct sidecar_tamper=rejected")


if __name__ == "__main__":
    main()
