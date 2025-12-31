# Envoy C++ Style Guide

C++20 기반. Google C++ Style Guide를 따르되 아래 항목은 다름.

## Naming Conventions

```cpp
// Functions: camelCase (lowercase start)
void doSomething();
bool isValid();

// Member variables: trailing underscore
int count_;
std::string name_;

// Constants
const int MaxRetries = 5;      // PascalCase
const int BUFFER_SIZE = 1024;  // SCREAMING_CASE (use sparingly)

// Enums: PascalCase
enum class LoadBalancer { RoundRobin, LeastRequest };
```

## Smart Pointer Aliases

```cpp
using FooPtr = std::unique_ptr<Foo>;
using BarSharedPtr = std::shared_ptr<Bar>;
using BlahConstSharedPtr = std::shared_ptr<const Blah>;
```

## Other Rules

- **Header guards**: Use `#pragma once`
- **Line limit**: 100 columns
- **Braces**: Required for all control statements (even single-line)
- **References over pointers**: When non-null is guaranteed
- **Thread annotations**: Use `ABSL_GUARDED_BY(mutex_)` for protected members
- **TODO format**: `TODO(github_username): description`
- **Exceptions**: Allowed on control plane, disallowed on data plane

## Inclusive Language

Required:
- allowlist (not whitelist)
- denylist/blocklist (not blacklist)
- primary/main (not master)
- secondary/replica (not slave)
