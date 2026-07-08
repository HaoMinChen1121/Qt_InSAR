"""Convert K&R brace style to Allman style for all .cpp/.h files."""

import os
import re
import glob

PROJECT_ROOT = r"F:\CSU_InSAR_ByCHM\Qt_InSAR\Qt_InSAR"

# Patterns to match K&R opening braces at end of line.
# Each is (regex, replacement) where replacement uses \1 \2 etc.
# Order matters: more specific patterns first.

def should_skip_line(line):
    """Return True if this line's '{' should NOT be moved (lambda, initializer, string, etc)."""
    stripped = line.strip()
    # Skip preprocessor directives
    if stripped.startswith('#'):
        return True
    # Skip single-line lambdas: [](...) { ... }  or  [=](...) { ... }
    if re.search(r'\[\s*[=&\w]*\s*\]\s*\([^)]*\)\s*\{', line):
        # Lambda on single line: don't touch
        return True
    # Skip lines where '{' appears in a string literal
    # Simple heuristic: if '{' is inside quotes
    if re.search(r'"[^"]*\{[^"]*"', line):
        return True
    # Skip initializer lists:  = { ... };
    if re.search(r'=\s*\{', line) and ';' in stripped:
        return True
    # Skip return { ... };
    if re.search(r'return\s+\{', stripped):
        return True
    # Skip array/struct initializers: Type var[] = { ... };
    if re.search(r'=\s*\{', stripped):
        return True
    # Skip inline empty braces at end of line:  {};
    if re.search(r'\{\s*\}\s*;?\s*$', line):
        return True
    # Skip lines where '{' is inside a // comment
    if re.search(r'//.*\{', line):
        return True
    return False

def has_brace_at_end(line):
    """Check if line has a K&R-style opening brace at the end."""
    stripped = line.rstrip()
    if not stripped.endswith('{'):
        return False
    # If the entire line is just '{', skip (already Allman)
    if stripped.strip() == '{':
        return False
    return True

def convert_line(line):
    """Convert a single line from K&R to Allman if needed.
    Returns list of lines (1 or 2)."""
    if not has_brace_at_end(line):
        return [line]

    if should_skip_line(line):
        return [line]

    # Find the indentation
    indent = ''
    for ch in line:
        if ch in ' \t':
            indent += ch
        else:
            break

    stripped = line.rstrip()
    brace_pos = stripped.rfind('{')
    before_brace = stripped[:brace_pos].rstrip()

    # Determine brace indentation.
    # For continuation lines (initializer lists starting with ':' or ','),
    # the brace should align with the function signature (one level less).
    content = before_brace.lstrip()
    if content.startswith(':') or content.startswith(','):
        # Reduce indentation by one level (4 spaces) for the brace
        if len(indent) >= 4:
            brace_indent = indent[:-4]
        else:
            brace_indent = ''
    else:
        brace_indent = indent

    new_lines = [indent + before_brace + '\n', brace_indent + '{\n']

    after_brace = stripped[brace_pos + 1:]
    if after_brace.strip():
        new_lines[1] = brace_indent + '{' + after_brace + '\n'

    return new_lines


def process_file(filepath):
    """Process a single file, converting K&R to Allman."""
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    new_lines = []
    changed = False

    for i, line in enumerate(lines):
        result = convert_line(line)
        if len(result) != 1:
            changed = True
        new_lines.extend(result)

    if changed:
        with open(filepath, 'w', encoding='utf-8', newline='\n') as f:
            f.writelines(new_lines)
        return True
    return False


def main():
    cpp_files = [f for f in glob.glob(os.path.join(PROJECT_ROOT, '**', '*.cpp'), recursive=True)
                 if 'x64' not in f and 'tools' not in f]
    h_files = [f for f in glob.glob(os.path.join(PROJECT_ROOT, '**', '*.h'), recursive=True)
               if 'x64' not in f and 'tools' not in f]

    all_files = cpp_files + h_files
    changed_count = 0

    for filepath in sorted(all_files):
        if process_file(filepath):
            changed_count += 1
            print(f"  Converted: {os.path.relpath(filepath, PROJECT_ROOT)}")

    print(f"\nDone. {changed_count} files converted out of {len(all_files)} total.")


if __name__ == '__main__':
    main()
