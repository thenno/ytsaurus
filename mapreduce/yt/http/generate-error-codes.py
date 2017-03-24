#!/usr/bin/env python
# -*- coding: utf-8 -*-

import argparse
import collections
import os
import re
import sys
import subprocess

#
# NOTE: if some need in adjustment of this script will ever appear
# maybe it would be easier to modify YT code, e.g. move all error_codes
# to files with specified name like error_codes and make them easier to parse.
#

PATTERN = 'DEFINE_ENUM(EErrorCode'

ErrorDescription = collections.namedtuple('ErrorDescription', ['name', 'code', 'comment'])

# namespace is tuple like ('NYT', 'NTableClient')
# error_list is list of ErrorDescription
ErrorClass = collections.namedtuple('ErrorClass', ['filename', 'namespace', 'error_list'])

def find_headers_with_error_codes(yt_path):
    fgrep_output = subprocess.check_output(
        ['fgrep',
         PATTERN,
         '--files-with-matches',
         '--recursive',
         yt_path])

    return [fname for fname in fgrep_output.split('\n') if fname]

def extract_namespace(text):
    result = []
    for match in re.finditer('namespace\s*(\S+)\s*{', text, re.MULTILINE):
        result.append(match.group(1))

    # we use heuristic here
    while 'NProto' in result:
        result.remove('NProto')
    while 'NLFAlloc' in result:
        result.remove('NLFAlloc')
    try:
        return (result[result.index('NYT') + 1],)
    except IndexError:
        return tuple()

def extract_error_class(filename):
    with open(filename) as inf:
        text = inf.read()

    enum_define_text = extract_enum_define_text(text)
    namespace = extract_namespace(text)
    error_list = parse_enum_define_text(enum_define_text)
    return ErrorClass(filename=filename, namespace=namespace, error_list=error_list)

def extract_enum_define_text(text):
    pos = text.find(PATTERN)
    assert pos >= 0
    pos = text.find('(', pos)
    assert pos >= 0

    depth = 1
    for c_index, c in enumerate(text[pos+1:], pos+1):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if depth == 0:
            return text[pos:c_index+1]
    else:
        raise RuntimeError, "Can't extract enum from can't find right brace"

def parse_enum_define_text(enum_text):
    result = []
    cur_comment = ""
    for line in enum_text.split('\n'):
        if re.search('^\s*//', line):
            cur_comment += line.strip() + '\n'
        else:
            m = re.match(
                r"""\s* \(
                        \( ([^)]*) \) # name
                        \s*
                        \( ([^)]*) \) # code
                    \) \s* """, line, re.VERBOSE)
            if m:
                result.append(
                    ErrorDescription(
                        comment=cur_comment,
                        name=m.group(1),
                        code=m.group(2)))
                cur_comment = ""
    return result

def generate_cpp_enum_text(error_class):
    result = ""

    result += "// from {}\n".format(error_class.filename)
    for namespace in error_class.namespace:
        result += "namespace {} {{\n".format(namespace)

    result += "\n"
    result += "////////////////////////////////////////////////////////////////////////////////\n"
    result += "\n"

    enum_description_list = error_class.error_list[:]
    enum_description_list.sort(key=lambda e: int(e.code))

    indent_str = ' ' * 4
    max_name_len = max(len(e.name) for e in enum_description_list)
    for e in enum_description_list:
        if e.comment:
            result += re.sub("^", indent_str, e.comment)
        indent_eq_str = ' ' * (max_name_len - len(e.name) + 1)
        result += '{indent}constexpr int {name}{indent_eq}= {code};\n'.format(
            indent=indent_str,
            name=e.name,
            indent_eq=indent_eq_str,
            code=e.code)

    result += "\n"
    result += "////////////////////////////////////////////////////////////////////////////////\n"
    result += "\n"

    for namespace in reversed(error_class.namespace):
        result += '}} // namespace {}\n'.format(namespace)

    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('yt_path', help='path to source root of yt')
    args = parser.parse_args()

    os.chdir(args.yt_path)
    header_path_list = find_headers_with_error_codes('.')

    error_class_list = []
    for header_path in header_path_list:
        error_class_list.append(extract_error_class(header_path))

    print "#pragma once"""
    print ""
    print "//"
    print "// generated by {}".format(os.path.basename(sys.argv[0]))
    print "//"

    print ""
    print "namespace NYT {"
    print "namespace NClusterErrorCodes {"
    print ""

    error_class_list.sort(key=lambda ec: int(ec.error_list[0].code))
    for error_class in error_class_list:
        print ""
        print ""
        print generate_cpp_enum_text(error_class)

    print "} // namespace NClusterErrorCodes"
    print "} // namespace NYT"

if __name__ == '__main__':
    main()
