import sys
import re
import os

def increment_build_number(file_path):
    try:
        with open(file_path, 'r') as f:
            content = f.read()

        match = re.search(r'#define\s+BUILD_NUMBER\s+(\d+)', content)
        if match:
            current_build = int(match.group(1))
            new_build = current_build + 1
            new_content = re.sub(r'(#define\s+BUILD_NUMBER\s+)\d+', f'\\g<1>{new_build}', content)
            
            with open(file_path, 'w') as f:
                f.write(new_content)
            
            print(f"Build number incremented to {new_build}")
        else:
            print("BUILD_NUMBER not found in file")
            
    except Exception as e:
        print(f"Error incrementing build number: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python increment_build.py <path_to_version_h>")
        sys.exit(1)
    
    version_file = sys.argv[1]
    increment_build_number(version_file)
