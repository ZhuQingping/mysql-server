#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# ===- clang-format-diff.py - ClangFormat Diff Reformatter ----*- python -*--===#
#
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
# ===------------------------------------------------------------------------===#

r"""
ClangFormat Diff Reformatter
============================
This script reads input from a unified diff and reformats all the changed
lines. This is useful to reformat all the lines touched by a specific patch.
Example usage for git/svn users:
  git diff -U0 HEAD^ | clang-format-diff.py -p1 -i
  svn diff --diff-cmd=diff -x-U0 | clang-format-diff.py -i
"""
import io
import argparse
import difflib
import glob
import os
import string
import subprocess
import sys


def main():
    proj_path = os.path.dirname(os.path.abspath(__file__))
    libstdc_path = os.path.join(proj_path, "/opt/hw/gcc-10.3/lib64")
    if os.path.exists(libstdc_path):
        os.environ['LD_LIBRARY_PATH'] = libstdc_path

    parser = argparse.ArgumentParser(description=
                                     'Reformat changed lines in diff. Without -i '
                                     'option just output the diff that would be '
                                     'introduced.')
    parser.add_argument('-fix', action='store_true', default=False,
                        help='apply edits to files instead of displaying a diff')
    parser.add_argument('-diff', action='store_true', default=False,
                        help='perform format on changed file')
    parser.add_argument('-p', metavar='NUM', default=0,
                        help='strip the smallest prefix containing P slashes')
    parser.add_argument('-regex', metavar='PATTERN', default=None,
                        help='custom pattern selecting file paths to reformat '
                             '(case sensitive, overrides -iregex)')
    parser.add_argument('-iregex', metavar='PATTERN', default=
    r'.*\.(cpp|cc|c\+\+|cxx|c|cl|h|hpp|m|mm|inc|js|ts|proto'
    r'|protodevel|java)',
                        help='custom pattern selecting file paths to reformat '
                             '(case insensitive, overridden by -regex)')
    parser.add_argument('-sort-includes', action='store_true', default=False,
                        help='let clang-format sort include blocks')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='be more verbose, ineffective without -i')
    # parser.add_argument('-style',
    #                     default='Google',
    #                     help='formatting style to apply (LLVM, Google, Chromium, '
    #                          'Mozilla, WebKit)')
    parser.add_argument('-binary',
                        default=os.path.join(proj_path, 'clang_check/bin/clang-format'),
                        help='location of binary to use for clang-format')
    parser.add_argument('files', nargs='*', default=[os.path.join(proj_path, 'sql')],
                        help='files to be processed e.g. src/util, src/util/*.cc')
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

    def run_git_command(command):
        try:
            # 在Python 2中需要手动处理字节串解码
            output = subprocess.check_output(
                command,
                stderr=subprocess.STDOUT
            )
            # 尝试UTF-8解码，失败时使用系统默认编码
            try:
                decoded = output.decode('utf-8')
            except UnicodeDecodeError:
                decoded = output.decode(sys.getdefaultencoding(), errors='replace')
            return decoded.splitlines()
        except subprocess.CalledProcessError as e:
            print("[Error] Command failed: git {}".format(' '.join(command)))
            try:
                err_msg = e.output.decode('utf-8', errors='replace')
            except AttributeError:  # 某些情况下output可能不存在
                err_msg = str(e)
            print("Reason: {}".format(err_msg.strip()))
            return []
        except Exception as e:
            print("[Critical] Unexpected error: {}".format(str(e)))
            return []

    def changed_files():
        changed = set()

        # 获取工作区修改
        changed.update(run_git_command(['git', 'diff', '--name-only']))

        # 获取暂存区修改
        changed.update(run_git_command(['git', 'diff', '--cached', '--name-only']))

        # 无修改时检查最近提交
        if not changed:
            last_commit = run_git_command(['git', 'diff', '--name-only', 'HEAD^', 'HEAD'])
            changed.update(last_commit)

        # 转换为绝对路径并过滤
        abs_paths = set()
        for f in changed:
            try:
                # 处理可能的非常规字符
                clean_path = f.strip('\r\n\x00')
                abs_path = os.path.abspath(clean_path)
                if os.path.exists(abs_path):
                    abs_paths.add(abs_path)
            except UnicodeEncodeError:
                print("[Warning] Skipped invalid path: {}".format(repr(f)))
            except Exception as e:
                print("[Warning] Path processing error: {}".format(str(e)))

        return abs_paths

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
            # if args.style:
            #     command.extend(['-style', args.style])
            command.append('-style=file')
            p = subprocess.Popen(command, stdout=subprocess.PIPE,
                                 stderr=None, stdin=subprocess.PIPE)
            stdout, stderr = p.communicate()
            if p.returncode != 0:
                sys.exit(p.returncode)

            if not args.fix:
                with open(filename) as f:
                    code = f.readlines()
                formatted_code = io.StringIO(stdout.decode('utf-8')).readlines()
                diff = difflib.unified_diff(code, formatted_code,
                                            filename, filename,
                                            '(before formatting)', '(after formatting)')
                diff_string = ''.join(diff)
                if len(diff_string) > 0:
                    sys.stdout.write(diff_string)


if __name__ == '__main__':
    main()
