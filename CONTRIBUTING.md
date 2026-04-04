# Contributing to ALP SDK

Thank you for your interest in contributing to the ALP SDK! This document provides guidelines for contributing.

## How to Contribute

### Reporting Bugs

1. Check if the issue already exists in [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
2. If not, [create a new issue](https://github.com/alplabai/alp-sdk/issues/new?template=bug_report.md) using the bug report template
3. Include: steps to reproduce, expected vs actual behavior, module type, SDK version

### Requesting Features

1. Check the [Feature Requests](https://community.alplab.ai/c/feature-requests/10) on the community forum
2. [Create a GitHub issue](https://github.com/alplabai/alp-sdk/issues/new?template=feature_request.md) or post on the forum
3. Describe the feature, your use case, and why it matters

### Submitting Code

1. Fork the repository
2. Create a feature branch from `main`: `git checkout -b feature/my-feature`
3. Make your changes following the code style below
4. Add or update tests for your changes
5. Ensure all tests pass: `cmake -B build -DBUILD_MOCK=ON -DBUILD_TESTS=ON && cmake --build build && cd build && ctest`
6. Commit with a descriptive message
7. Push to your fork and open a Pull Request

### Code Style

- Follow existing code conventions in the project
- Use `alp_` prefix for all public API functions
- Keep functions focused and well-documented
- Add unit tests for new functionality using the Mock driver

## Development Setup

```bash
git clone https://github.com/alplabai/alp-sdk.git
cd alp-sdk
cmake -B build -DBUILD_MOCK=ON -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Getting Help

- [Documentation](https://docs.alplab.ai)
- [Community Forum](https://community.alplab.ai)
- [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
