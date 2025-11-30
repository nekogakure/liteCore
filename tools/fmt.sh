find src/kernel src/user src/apps boot -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +
