#!/usr/bin/env python3
"""Exercise the actual voice HTTP transports with a deterministic ESP-IDF mock."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf-path', type=Path, default=os.environ.get('IDF_PATH'))
    parser.add_argument('--timing-include', type=Path)
    args = parser.parse_args()
    if not args.idf_path:
        parser.error('Set IDF_PATH or --idf-path for cJSON')
    root = Path(__file__).resolve().parents[1]
    cjson = args.idf_path / 'components/json/cJSON'
    with tempfile.TemporaryDirectory(prefix='voice-http-') as directory:
        work = Path(directory)
        subprocess.run(['cc', '-c', str(cjson / 'cJSON.c'), '-o', str(work / 'cjson.o')], check=True)
        includes = [root / 'tests/voice_http_shim', root / 'tests/remote_shim', root / 'main', cjson]
        if args.timing_include:
            includes.append(args.timing_include)
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g']
        command += ['-I' + str(path) for path in includes]
        command += [str(root / 'tests/voice_http_test.cpp')]
        command += [str(root / 'main' / name) for name in (
            'voice_http.cpp', 'voice_transport.cpp', 'voice_response_transport.cpp',
            'voice_page_transport.cpp', 'voice_protocol.cpp')]
        command += [str(work / 'cjson.o'), '-o', str(work / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    main()
