cd ..
find . -path ./third-party -prune -o -name "*.cc" -o -name "*.hpp" -o -name "*.h" -print | xargs clang-format -i