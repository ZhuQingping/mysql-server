#!/usr/bin/env python
# Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

r"""
Check code format: python code_format.py xx.cc xx.h
auto format code:  python code_format.py xx.cc xx.h -fix
"""
from io import BytesIO,TextIOWrapper,StringIO
import argparse
import difflib
import glob
import os
import string
import subprocess
import sys


def main():
    proj_path = os.path.dirname(os.path.abspath(__file__))

    parser = argparse.ArgumentParser(description=
                                     'Reformat changed lines in diff. Without -i '
                                     'option just output the diff that would be '
                                     'introduced.')
    parser.add_argument('-fix', action='store_true', default=False,
                        help='apply edits to files instead of displaying a diff')
    parser.add_argument('-diff', action='store_true', default=False,
                        help='perform format on changed file')
    parser.add_argument('-binary',
                        default= 'clang-format',
                        help='location of binary to use for clang-format')
    parser.add_argument('-v', '--verbose', action='store_true',help='')
    parser.add_argument('-iregex', metavar='PATTERN', default=
    r'.*\.(cpp|cc|c\+\+|cxx|c|cl|h|hpp|m|mm|inc|js|ts|proto'
    r'|protodevel|java)', help='')
    parser.add_argument('files', nargs='*', default=[os.path.join(proj_path, 'src')],
                        help='files to be processed e.g. src/util, src/util/*.cc')
    parser.add_argument('-sort-includes', action='store_true', default=False, help='')
    args = parser.parse_args()

    def get_all_files(dir, suffixes=('.cc', '.h'), ignore=('.pb.cc', '.pb.h', '.json.h')):
        def valid(path):
            if any(path.endswith(suffix) for suffix in ignore):
                return False
            if any(path.endswith(suffix) for suffix in suffixes):
                return True
            return False

        files = []
        for dirpath, dirnames, filenames in os.walk(dir):
            files.extend([os.path.join(dirpath, filename) for filename in filenames if valid(filename)])
        return files

    def parse_patten(pattern):
        files = []
        if os.path.isdir(pattern):
            files = get_all_files(pattern)
        else:
            files = glob.glob(pattern)
        return files

    def changed_files():
        changed = set(subprocess.check_output(['git', 'diff', '--name-only']).split())
        changed.update(subprocess.check_output(['git', 'diff', '--cached', '--name-only']).split())
        if len(changed) == 0:
            changed = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD^', 'HEAD']).split())
        changed = set(map(os.path.abspath, changed))
        return changed

    changed = changed_files()
    for pattern in args.files:
        for filename in parse_patten(pattern):
            if args.diff and os.path.abspath(filename) not in changed:
                continue
            if args.fix and args.verbose:
                print('Formatting', filename)
            command = [args.binary, filename]
            if args.fix:
                command.append('-i')
            if args.sort_includes:
                command.append('-sort-includes')

            command.append('-style=file')
            p = subprocess.Popen(command, stdout=subprocess.PIPE,
                                 stderr=None, stdin=subprocess.PIPE)
            stdout, stderr = p.communicate()
            if p.returncode != 0:
                sys.exit(p.returncode);

            if not args.fix:
                with open(filename) as f:
                    code = f.readlines()
                formatted_code = TextIOWrapper(BytesIO(stdout), encoding='utf-8').readlines()
                diff = difflib.unified_diff(code, formatted_code,
                                            filename, filename,
                                            '(before formatting)', '(after formatting)')
                diff_string = '\n'.join(diff)
                if len(diff_string) > 0:
                    sys.stdout.write(diff_string)


if __name__ == '__main__':
    main()
