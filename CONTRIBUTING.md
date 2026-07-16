# Contributing

## Local workflow

1. Create a focused branch.
2. Write or update a failing test before implementation.
3. Make the smallest change that passes the test.
4. Run the full verification set below.
5. Use a conventional commit such as `feat: add controller telemetry command`.

## Quality gates

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python -m ruff check python tests
python -m coverage run --branch -m unittest discover -s tests -p "test_*.py"
python -m coverage report --include="python/*" --fail-under=80
```

New behavior needs unit coverage and, when it crosses the TCP/process boundary, an integration or E2E test. Keep externally supplied data bounded and validated. Do not add credentials or production endpoints to the repository.
