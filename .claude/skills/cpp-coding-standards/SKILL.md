---
name: cpp-coding-standards
description: C++ coding standards based on the C++ Core Guidelines (isocpp.github.io). Use when writing, reviewing, or refactoring C++ code to enforce modern, safe, and idiomatic practices.
origin: ECC
---

# C++ Coding Standards (C++ Core Guidelines)

Comprehensive coding standards for modern C++ (C++17/20/23) derived from the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines). Enforces type safety, resource safety, immutability, and clarity.

## When to Use

- Writing new C++ code (classes, functions, templates)
- Reviewing or refactoring existing C++ code
- Making architectural decisions in C++ projects
- Enforcing consistent style across a C++ codebase
- Choosing between language features (e.g., `enum` vs `enum class`, raw pointer vs smart pointer)

### When NOT to Use

- Non-C++ projects
- Legacy C codebases that cannot adopt modern C++ features
- Embedded/bare-metal contexts where specific guidelines conflict with hardware constraints (adapt selectively)

---

# Prefer C++ to C

**Core Guideline — CPL.1**

> The presence of classes, templates, namespaces, `std::vector`, or smart
> pointers does not by itself make a design idiomatic C++.

The primary goal of this skill is:

> Write and review C++ as C++, not as C with classes.

## Definition: C with classes

"C with classes" is procedural C-style state and lifetime management that
has merely been wrapped in C++ syntax.

### Suspicious design patterns

These are design smells, not unconditional syntax bans:

* `init()` / `deinit()` or `init()` / `shutdown()` protocols that permit
  partially initialized objects
* public or weakly encapsulated state bags manipulated by procedural functions
* `bool`, integer, or status-code returns combined with output parameters
* pointer-plus-length interfaces where a range abstraction is appropriate
* owning handles represented as plain integers or raw pointers with cleanup
  performed elsewhere
* manual cleanup paths
* `goto cleanup`
* repeated `if (resource) release(resource)` logic
* explicit lifecycle flags such as `initialized_`, `opened_`, or `started_`
  used primarily to compensate for invalid object states
* `void*` context pointers and C callback conventions inside normal C++ code
* macro-based constants or dispatch where typed language facilities apply
* raw arrays used as ordinary containers
* manual memory ownership
* pervasive pointer semantics where value semantics are natural
* large switch statements over type tags that recreate an object model
* classes that are only namespaces for procedural functions
* classes whose methods merely mutate a shared context structure
* manager objects that centralize unrelated lifetime and mutable state
* manual lock/unlock
* manual thread lifetime management without an owning abstraction
* C string handling in code that naturally owns or observes text
* `memset` used as object initialization
* `memcpy` used to copy non-trivial object state
* C-style variadic interfaces
* C-style casts
* weakly typed integer flags and mode parameters
* sentinel values where the type system can express absence or state
* APIs that require callers to remember a cleanup or finalization operation

### Legitimate C-like code

Low-level boundaries, operating-system APIs, foreign-function interfaces,
wire formats, memory-mapped structures, SIMD code, allocator internals, and
other deliberately low-level components may legitimately use C-like
representations.

When C-like code is required:

1. identify the low-level boundary
2. keep it narrow
3. document the invariant
4. prevent ownership and lifetime rules from leaking outward
5. expose a safer C++ interface to ordinary callers when practical

Do not mechanically rewrite a valid low-level boundary merely to make it look
more object-oriented.

---

# Cross-Cutting Principles

These themes recur across the entire guidelines and form the foundation:

1. **RAII everywhere** (P.8, R.1, E.6, CP.20): Bind resource lifetime to object lifetime
2. **Immutability by default** (P.10, Con.1-5, ES.25): Start with `const`/`constexpr`; mutability is the exception
3. **Type safety** (P.4, I.4, ES.46-49, Enum.3): Use the type system to prevent errors at compile time
4. **Express intent** (P.3, F.1, NL.1-2, T.10): Names, types, and concepts should communicate purpose
5. **Minimize complexity** (F.2-3, ES.5, Per.4-5): Simple code is correct code
6. **Value semantics over pointer semantics** (C.10, R.3-5, F.20, CP.31): Prefer returning by value and scoped objects

---

# C++ Design Synthesis Gate

Before implementing a new component, determine:

### 1. What is the resource?

Identify resources such as:

* memory
* file descriptors
* sockets
* file handles
* mappings
* locks
* threads
* worker pools
* registrations
* subscriptions
* backend contexts

Ask:

> Can the resource lifetime be represented by an object lifetime?

Prefer RAII.

### 2. What is the valid state?

Identify the component invariant.

Ask:

> Can construction produce a valid object immediately?

Prefer constructors or factories that return complete valid objects.

Avoid two-phase initialization unless required by a documented constraint.

Do not create an object that requires callers to remember `init()` before any
other operation when the type can prevent that state.

### 3. What is the ownership model?

Explicitly distinguish:

* owning value
* unique ownership
* shared ownership
* borrowed reference
* nullable observer
* contiguous range
* string ownership
* string observation

Use types to communicate these distinctions.

Do not use one pointer representation for multiple unrelated semantics.

### 4. What is the natural value model?

Ask:

> Should this concept behave as a value?

Prefer value semantics when identity and shared mutation are not essential.

Do not introduce heap allocation merely to make an object polymorphic or
shareable.

Do not use `shared_ptr` as a substitute for an ownership decision.

### 5. What should the interface return?

Prefer complete return values.

Prefer a named result type when an operation returns multiple related values.

Avoid output parameters as the default interface design.

Distinguish a real mutation operation from a C-style function that writes
results through pointers or references.

### 6. Is this a range?

For contiguous sequences and buffers, distinguish:

* ownership
* mutable observation
* immutable observation

Prefer appropriate C++ range abstractions such as `std::span` when supported
by the project's C++ standard and when the interface represents a contiguous
range.

Do not default to `(T*, size)` merely because the implementation ultimately
calls a C or operating-system API.

Keep pointer-plus-length representation at the low-level boundary when
possible.

### 7. Is invalid state representable?

Ask:

> Can a caller construct or observe a meaningless state?

Use types, constructors, scoped enums, variants, optional state, or separate
types when they materially prevent misuse.

Do not solve a type-model problem with comments and boolean flags by default.

### 8. Is polymorphism actually needed?

Do not automatically introduce inheritance.

Consider:

* value semantics
* composition
* templates
* concepts
* variants
* callable objects
* type erasure
* ordinary functions

Use inheritance when runtime subtype polymorphism is actually the intended
model.

The goal is C++ abstraction, not maximal object orientation.

### 9. What is the failure model?

Follow the project's documented error model.

Do not mechanically impose exceptions on a project using another deliberate
error model.

Regardless of representation, ensure:

* cleanup is automatic
* partial state is controlled
* errors are not silently swallowed
* the interface communicates failure semantics

### 10. What performs cleanup?

The preferred answer should normally be:

> the owning object's destructor

Treat answers such as:

* "the caller"
* "shutdown()"
* "cleanup()"
* "the error label"
* "whoever allocated it"

as signals to inspect the design more closely.

---

# Anti-C-With-Classes Review

Before marking C++ implementation work complete, run an explicit review.

Ask:

1. Did I model a resource as an object with deterministic lifetime?
2. Did I introduce an `init`/`shutdown` protocol that RAII could replace?
3. Can the object exist in a meaningless partially initialized state?
4. Did I use a state flag to compensate for a weak invariant?
5. Did I expose pointer-plus-length instead of a range abstraction?
6. Did I use output parameters where a return value would communicate intent?
7. Did I use raw ownership or external cleanup?
8. Did I represent domain states as integers, flags, macros, or magic values?
9. Did I write a manager/context object that is effectively a C state struct?
10. Are methods merely procedural functions operating on that state struct?
11. Did I recreate manual dispatch with type tags and large switches?
12. Did I use shared ownership because ownership was unclear?
13. Did I use inheritance simply to obtain dynamic dispatch?
14. Did I make callers remember an ordering protocol the type system could
    express?
15. Did I keep a C or OS abstraction leaking through public C++ interfaces?
16. Would this design still look essentially identical if all classes were
    converted to C structs and methods to free functions?

The final question is particularly important.

If the answer is yes, determine whether the component is intentionally a
low-level C boundary.

If not, redesign it using C++ facilities and Core Guidelines principles.

Do not treat this review as a demand to introduce inheritance or elaborate
design patterns.

Prefer the simplest type-safe, lifetime-safe C++ design.

---

# Contrast Examples

## Resource lifetime

### Bad: C with classes — protocol-dependent state

```cpp
class File {
public:
    bool open(const char* path);
    void close();

    // Caller must remember: do not call read() before open().
    // Do not call close() twice.
    // Do not use the object after close().
    // The destructor does nothing.
    int read(char* buf, std::size_t n);

private:
    int fd_{-1};
    bool opened_{false};
};
```

This design permits a partially initialized, meaningless state. The
object's invariant is not enforced. Callers must remember a protocol.

### Good: RAII — construction produces a valid object

```cpp
class File {
public:
    explicit File(const char* path, FileOpen mode = FileOpen::ReadOnly)
        : fd_(::open(path, static_cast<int>(mode))) {
        if (fd_ < 0) {
            throw std::system_error(errno, std::system_category(), path);
        }
    }

    ~File() {
        if (fd_ >= 0) ::close(fd_);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int fd() const { return fd_; }

private:
    int fd_;
};
```

Construction always produces a valid open file or throws. Destruction
always closes. No `init()`, no `close()`, no lifecycle flags.

## Buffer interface

### Bad: pointer-plus-length with output parameter

```cpp
int write_data(const unsigned char* data,
               std::size_t size,
               std::size_t* written);
```

This is a C-style interface. Callers must pass correct pointer and length.
The result is written through an output parameter. Ownership of the written
count is unclear.

### Good: range and result type

```cpp
struct WriteResult {
    std::size_t bytes_written;
    bool success;
};

WriteResult write_data(std::span<const std::byte> data) {
    // data.size() is always valid, no null-pointer risk.
    // The result is returned, not written through a pointer.
    // ...
}
```

## Procedural context object

### Bad: C state struct wrapped in a class

```cpp
struct PoolContext {
    Thread* threads;
    std::size_t thread_count;
    Job* jobs;
    std::size_t job_count;
    bool stopping;
};

bool pool_init(PoolContext*);
bool pool_submit(PoolContext*, Job*);
void pool_shutdown(PoolContext*);
void pool_destroy(PoolContext*);
```

This is C with classes. The class is a state bag. Methods are procedural
functions operating on shared state. Callers must call `pool_init` before
`pool_submit` and `pool_shutdown` before `pool_destroy`. The type system
does not prevent misuse.

### Good: C++ design with explicit ownership

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count)
        : stopping_(false) {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(Job job) {
        {
            std::scoped_lock lock(mutex_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void worker_loop() { /* ... */ }

    std::vector<std::thread> workers_;
    std::queue<Job> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_;
};
```

Ownership is explicit. Worker lifetime is owned by the object. Shutdown
belongs to the destructor. The object invariant is meaningful: either the
pool is running or it is stopped.

## Type-tag dispatch

### Bad: tag plus union plus switch

```cpp
enum class ObjectType { Null, Int, Float, String, Array, Map };

struct Value {
    ObjectType type;
    union {
        int int_val;
        double float_val;
        char* string_val;
        // ...
    };
};

void process(const Value& v) {
    switch (v.type) {
        case ObjectType::Int:    /* ... */ break;
        case ObjectType::Float:  /* ... */ break;
        case ObjectType::String: /* ... */ break;
        // ...
        default: break;
    }
}
```

This recreates an object model with type tags. The compiler cannot check
exhaustiveness of all operations. Callers must remember to handle every tag.

### Good: C++ alternative

Choose the appropriate alternative depending on the problem:

**Closed set, runtime dispatch — use `std::variant`:**

```cpp
using Value = std::variant<
    std::monostate,
    int,
    double,
    std::string,
    std::vector<Value>,
    std::map<std::string, Value>
>;

void process(const Value& v) {
    std::visit([](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) { /* null */ }
        else if constexpr (std::is_same_v<T, int>) { /* int */ }
        // compiler checks exhaustiveness
    }, v);
}
```

**Open set, runtime dispatch — use inheritance:**

```cpp
class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor&) = 0;
};
```

**Compile-time dispatch — use templates/concepts:**

```cpp
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template<Numeric T>
T add(T a, T b) { return a + b; }
```

## Error plus output parameter

### Bad: status code and out-parameter

```cpp
ErrorCode parse_config(const char* path, Config* out) {
    if (!path) return ErrorCode::InvalidArg;
    if (!out) return ErrorCode::InvalidArg;
    // ... parse ...
    out->timeout = 30;
    out->retries = 3;
    return ErrorCode::Ok;
}

Config config;
ErrorCode err = parse_config("/etc/app.conf", &config);
if (err != ErrorCode::Ok) { /* handle error */ }
```

### Good: complete return type

```cpp
std::expected<Config, ConfigError> parse_config(std::string_view path) {
    // ... parse ...
    Config config;
    config.timeout = std::chrono::seconds{30};
    config.retries = 3;
    return config;
}

auto result = parse_config("/etc/app.conf");
if (!result) {
    // handle result.error()
}
// use *result or result.value()
```

If the project does not use exceptions and does not have `std::expected`,
use a project-specific result type. The principle remains: return the
result, do not write it through a pointer.

---

# Guidelines Coverage

## Philosophy & Interfaces (P.*, I.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **P.1** | Express ideas directly in code |
| **P.3** | Express intent |
| **P.4** | Ideally, a program should be statically type safe |
| **P.5** | Prefer compile-time checking to run-time checking |
| **P.8** | Don't leak any resources |
| **P.10** | Prefer immutable data to mutable data |
| **I.1** | Make interfaces explicit |
| **I.2** | Avoid non-const global variables |
| **I.4** | Make interfaces precisely and strongly typed |
| **I.11** | Never transfer ownership by a raw pointer or reference |
| **I.23** | Keep the number of function arguments low |

### DO

```cpp
// P.10 + I.4: Immutable, strongly typed interface
struct Temperature {
    double kelvin;
};

Temperature boil(const Temperature& water);
```

### DON'T

```cpp
// Weak interface: unclear ownership, unclear units
double boil(double* temp);

// Non-const global variable
int g_counter = 0;  // I.2 violation
```

## Functions (F.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **F.1** | Package meaningful operations as carefully named functions |
| **F.2** | A function should perform a single logical operation |
| **F.3** | Keep functions short and simple |
| **F.4** | If a function might be evaluated at compile time, declare it `constexpr` |
| **F.6** | If your function must not throw, declare it `noexcept` |
| **F.8** | Prefer pure functions |
| **F.16** | For "in" parameters, pass cheaply-copied types by value and others by `const&` |
| **F.20** | For "out" values, prefer return values to output parameters |
| **F.21** | To return multiple "out" values, prefer returning a struct |
| **F.43** | Never return a pointer or reference to a local object |

### Parameter Passing

```cpp
// F.16: Cheap types by value, others by const&
void print(int x);                           // cheap: by value
void analyze(const std::string& data);       // expensive: by const&
void transform(std::string s);               // sink: by value (will move)

// F.20 + F.21: Return values, not output parameters
struct ParseResult {
    std::string token;
    int position;
};

ParseResult parse(std::string_view input);   // GOOD: return struct
```

### Pure Functions and constexpr

```cpp
// F.4 + F.8: Pure, constexpr where possible
constexpr int factorial(int n) noexcept {
    return (n <= 1) ? 1 : n * factorial(n - 1);
}

static_assert(factorial(5) == 120);
```

### Anti-Patterns

- Returning `T&&` from functions (F.45)
- Using `va_arg` / C-style variadics (F.55)
- Capturing by reference in lambdas passed to other threads (F.53)
- Returning `const T` which inhibits move semantics (F.49)

## Classes & Class Hierarchies (C.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **C.2** | Use `class` if invariant exists; `struct` if data members vary independently |
| **C.9** | Minimize exposure of members |
| **C.20** | If you can avoid defining default operations, do (Rule of Zero) |
| **C.21** | If you define or `=delete` any copy/move/destructor, handle them all (Rule of Five) |
| **C.35** | Base class destructor: public virtual or protected non-virtual |
| **C.41** | A constructor should create a fully initialized object |
| **C.46** | Declare single-argument constructors `explicit` |
| **C.67** | A polymorphic class should suppress public copy/move |
| **C.128** | Virtual functions: specify exactly one of `virtual`, `override`, or `final` |

### Rule of Zero

```cpp
// C.20: Let the compiler generate special members
struct Employee {
    std::string name;
    std::string department;
    int id;
    // No destructor, copy/move constructors, or assignment operators needed
};
```

### Rule of Five

```cpp
// C.21: If you must manage a resource, define all five
class Buffer {
public:
    explicit Buffer(std::size_t size)
        : data_(std::make_unique<char[]>(size)), size_(size) {}

    ~Buffer() = default;

    Buffer(const Buffer& other)
        : data_(std::make_unique<char[]>(other.size_)), size_(other.size_) {
        std::copy_n(other.data_.get(), size_, data_.get());
    }

    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            auto new_data = std::make_unique<char[]>(other.size_);
            std::copy_n(other.data_.get(), other.size_, new_data.get());
            data_ = std::move(new_data);
            size_ = other.size_;
        }
        return *this;
    }

    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept = default;

private:
    std::unique_ptr<char[]> data_;
    std::size_t size_;
};
```

### Class Hierarchy

```cpp
// C.35 + C.128: Virtual destructor, use override
class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;  // C.121: pure interface
};

class Circle : public Shape {
public:
    explicit Circle(double r) : radius_(r) {}
    double area() const override { return 3.14159 * radius_ * radius_; }

private:
    double radius_;
};
```

### Anti-Patterns

- Calling virtual functions in constructors/destructors (C.82)
- Using `memset`/`memcpy` on non-trivial types (C.90)
- Providing different default arguments for virtual function and overrider (C.140)
- Making data members `const` or references, which suppresses move/copy (C.12)

## Resource Management (R.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **R.1** | Manage resources automatically using RAII |
| **R.3** | A raw pointer (`T*`) is non-owning |
| **R.5** | Prefer scoped objects; don't heap-allocate unnecessarily |
| **R.10** | Avoid `malloc()`/`free()` |
| **R.11** | Avoid calling `new` and `delete` explicitly |
| **R.20** | Use `unique_ptr` or `shared_ptr` to represent ownership |
| **R.21** | Prefer `unique_ptr` over `shared_ptr` unless sharing ownership |
| **R.22** | Use `make_shared()` to make `shared_ptr`s |

### Smart Pointer Usage

```cpp
// R.11 + R.20 + R.21: RAII with smart pointers
auto widget = std::make_unique<Widget>("config");  // unique ownership
auto cache  = std::make_shared<Cache>(1024);        // shared ownership

// R.3: Raw pointer = non-owning observer
void render(const Widget* w) {  // does NOT own w
    if (w) w->draw();
}

render(widget.get());
```

### RAII Pattern

```cpp
// R.1: Resource acquisition is initialization
class FileHandle {
public:
    explicit FileHandle(const std::string& path)
        : handle_(std::fopen(path.c_str(), "r")) {
        if (!handle_) throw std::runtime_error("Failed to open: " + path);
    }

    ~FileHandle() {
        if (handle_) std::fclose(handle_);
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) std::fclose(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

private:
    std::FILE* handle_;
};
```

### Anti-Patterns

- Naked `new`/`delete` (R.11)
- `malloc()`/`free()` in C++ code (R.10)
- Multiple resource allocations in a single expression (R.13 -- exception safety hazard)
- `shared_ptr` where `unique_ptr` suffices (R.21)

## Expressions & Statements (ES.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **ES.5** | Keep scopes small |
| **ES.20** | Always initialize an object |
| **ES.23** | Prefer `{}` initializer syntax |
| **ES.25** | Declare objects `const` or `constexpr` unless modification is intended |
| **ES.28** | Use lambdas for complex initialization of `const` variables |
| **ES.45** | Avoid magic constants; use symbolic constants |
| **ES.46** | Avoid narrowing/lossy arithmetic conversions |
| **ES.47** | Use `nullptr` rather than `0` or `NULL` |
| **ES.48** | Avoid casts |
| **ES.50** | Don't cast away `const` |

### Initialization

```cpp
// ES.20 + ES.23 + ES.25: Always initialize, prefer {}, default to const
const int max_retries{3};
const std::string name{"widget"};
const std::vector<int> primes{2, 3, 5, 7, 11};

// ES.28: Lambda for complex const initialization
const auto config = [&] {
    Config c;
    c.timeout = std::chrono::seconds{30};
    c.retries = max_retries;
    c.verbose = debug_mode;
    return c;
}();
```

### Anti-Patterns

- Uninitialized variables (ES.20)
- Using `0` or `NULL` as pointer (ES.47 -- use `nullptr`)
- C-style casts (ES.48 -- use `static_cast`, `const_cast`, etc.)
- Casting away `const` (ES.50)
- Magic numbers without named constants (ES.45)
- Mixing signed and unsigned arithmetic (ES.100)
- Reusing names in nested scopes (ES.12)

## Error Handling (E.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **E.1** | Develop an error-handling strategy early in a design |
| **E.2** | Throw an exception to signal that a function can't perform its assigned task |
| **E.6** | Use RAII to prevent leaks |
| **E.12** | Use `noexcept` when throwing is impossible or unacceptable |
| **E.14** | Use purpose-designed user-defined types as exceptions |
| **E.15** | Throw by value, catch by reference |
| **E.16** | Destructors, deallocation, and swap must never fail |
| **E.17** | Don't try to catch every exception in every function |

### Exception Hierarchy

```cpp
// E.14 + E.15: Custom exception types, throw by value, catch by reference
class AppError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class NetworkError : public AppError {
public:
    NetworkError(const std::string& msg, int code)
        : AppError(msg), status_code(code) {}
    int status_code;
};

void fetch_data(const std::string& url) {
    // E.2: Throw to signal failure
    throw NetworkError("connection refused", 503);
}

void run() {
    try {
        fetch_data("https://api.example.com");
    } catch (const NetworkError& e) {
        log_error(e.what(), e.status_code);
    } catch (const AppError& e) {
        log_error(e.what());
    }
    // E.17: Don't catch everything here -- let unexpected errors propagate
}
```

### Anti-Patterns

- Throwing built-in types like `int` or string literals (E.14)
- Catching by value (slicing risk) (E.15)
- Empty catch blocks that silently swallow errors
- Using exceptions for flow control (E.3)
- Error handling based on global state like `errno` (E.28)

## Constants & Immutability (Con.*)

### All Rules

| Rule | Summary |
|------|---------|
| **Con.1** | By default, make objects immutable |
| **Con.2** | By default, make member functions `const` |
| **Con.3** | By default, pass pointers and references to `const` |
| **Con.4** | Use `const` for values that don't change after construction |
| **Con.5** | Use `constexpr` for values computable at compile time |

```cpp
// Con.1 through Con.5: Immutability by default
class Sensor {
public:
    explicit Sensor(std::string id) : id_(std::move(id)) {}

    // Con.2: const member functions by default
    const std::string& id() const { return id_; }
    double last_reading() const { return reading_; }

    // Only non-const when mutation is required
    void record(double value) { reading_ = value; }

private:
    const std::string id_;  // Con.4: never changes after construction
    double reading_{0.0};
};

// Con.3: Pass by const reference
void display(const Sensor& s) {
    std::cout << s.id() << ": " << s.last_reading() << '\n';
}

// Con.5: Compile-time constants
constexpr double PI = 3.14159265358979;
constexpr int MAX_SENSORS = 256;
```

## Concurrency & Parallelism (CP.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **CP.2** | Avoid data races |
| **CP.3** | Minimize explicit sharing of writable data |
| **CP.4** | Think in terms of tasks, rather than threads |
| **CP.8** | Don't use `volatile` for synchronization |
| **CP.20** | Use RAII, never plain `lock()`/`unlock()` |
| **CP.21** | Use `std::scoped_lock` to acquire multiple mutexes |
| **CP.22** | Never call unknown code while holding a lock |
| **CP.42** | Don't wait without a condition |
| **CP.44** | Remember to name your `lock_guard`s and `unique_lock`s |
| **CP.100** | Don't use lock-free programming unless you absolutely have to |

### Safe Locking

```cpp
// CP.20 + CP.44: RAII locks, always named
class ThreadSafeQueue {
public:
    void push(int value) {
        std::lock_guard<std::mutex> lock(mutex_);  // CP.44: named!
        queue_.push(value);
        cv_.notify_one();
    }

    int pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        // CP.42: Always wait with a condition
        cv_.wait(lock, [this] { return !queue_.empty(); });
        const int value = queue_.front();
        queue_.pop();
        return value;
    }

private:
    std::mutex mutex_;             // CP.50: mutex with its data
    std::condition_variable cv_;
    std::queue<int> queue_;
};
```

### Multiple Mutexes

```cpp
// CP.21: std::scoped_lock for multiple mutexes (deadlock-free)
void transfer(Account& from, Account& to, double amount) {
    std::scoped_lock lock(from.mutex_, to.mutex_);
    from.balance_ -= amount;
    to.balance_ += amount;
}
```

### Anti-Patterns

- `volatile` for synchronization (CP.8 -- it's for hardware I/O only)
- Detaching threads (CP.26 -- lifetime management becomes nearly impossible)
- Unnamed lock guards: `std::lock_guard<std::mutex>(m);` destroys immediately (CP.44)
- Holding locks while calling callbacks (CP.22 -- deadlock risk)
- Lock-free programming without deep expertise (CP.100)

## Templates & Generic Programming (T.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **T.1** | Use templates to raise the level of abstraction |
| **T.2** | Use templates to express algorithms for many argument types |
| **T.10** | Specify concepts for all template arguments |
| **T.11** | Use standard concepts whenever possible |
| **T.13** | Prefer shorthand notation for simple concepts |
| **T.43** | Prefer `using` over `typedef` |
| **T.120** | Use template metaprogramming only when you really need to |
| **T.144** | Don't specialize function templates (overload instead) |

### Concepts (C++20)

```cpp
#include <concepts>

// T.10 + T.11: Constrain templates with standard concepts
template<std::integral T>
T gcd(T a, T b) {
    while (b != 0) {
        a = std::exchange(b, a % b);
    }
    return a;
}

// T.13: Shorthand concept syntax
void sort(std::ranges::random_access_range auto& range) {
    std::ranges::sort(range);
}

// Custom concept for domain-specific constraints
template<typename T>
concept Serializable = requires(const T& t) {
    { t.serialize() } -> std::convertible_to<std::string>;
};

template<Serializable T>
void save(const T& obj, const std::string& path);
```

### Anti-Patterns

- Unconstrained templates in visible namespaces (T.47)
- Specializing function templates instead of overloading (T.144)
- Template metaprogramming where `constexpr` suffices (T.120)
- `typedef` instead of `using` (T.43)

## Standard Library (SL.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **SL.1** | Use libraries wherever possible |
| **SL.2** | Prefer the standard library to other libraries |
| **SL.con.1** | Prefer `std::array` or `std::vector` over C arrays |
| **SL.con.2** | Prefer `std::vector` by default |
| **SL.str.1** | Use `std::string` to own character sequences |
| **SL.str.2** | Use `std::string_view` to refer to character sequences |
| **SL.io.50** | Avoid `endl` (use `'\n'` -- `endl` forces a flush) |

```cpp
// SL.con.1 + SL.con.2: Prefer vector/array over C arrays
const std::array<int, 4> fixed_data{1, 2, 3, 4};
std::vector<std::string> dynamic_data;

// SL.str.1 + SL.str.2: string owns, string_view observes
std::string build_greeting(std::string_view name) {
    return "Hello, " + std::string(name) + "!";
}

// SL.io.50: Use '\n' not endl
std::cout << "result: " << value << '\n';
```

## Enumerations (Enum.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **Enum.1** | Prefer enumerations over macros |
| **Enum.3** | Prefer `enum class` over plain `enum` |
| **Enum.5** | Don't use ALL_CAPS for enumerators |
| **Enum.6** | Avoid unnamed enumerations |

```cpp
// Enum.3 + Enum.5: Scoped enum, no ALL_CAPS
enum class Color { red, green, blue };
enum class LogLevel { debug, info, warning, error };

// BAD: plain enum leaks names, ALL_CAPS clashes with macros
enum { RED, GREEN, BLUE };           // Enum.3 + Enum.5 + Enum.6 violation
#define MAX_SIZE 100                  // Enum.1 violation -- use constexpr
```

## Source Files & Naming (SF.*, NL.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **SF.1** | Use `.cpp` for code files and `.h` for interface files |
| **SF.7** | Don't write `using namespace` at global scope in a header |
| **SF.8** | Use `#include` guards for all `.h` files |
| **SF.11** | Header files should be self-contained |
| **NL.5** | Avoid encoding type information in names (no Hungarian notation) |
| **NL.8** | Use a consistent naming style |
| **NL.9** | Use ALL_CAPS for macro names only |
| **NL.10** | Prefer `underscore_style` names |

### Header Guard

```cpp
// SF.8: Include guard (or #pragma once)
#ifndef PROJECT_MODULE_WIDGET_H
#define PROJECT_MODULE_WIDGET_H

// SF.11: Self-contained -- include everything this header needs
#include <string>
#include <vector>

namespace project::module {

class Widget {
public:
    explicit Widget(std::string name);
    const std::string& name() const;

private:
    std::string name_;
};

}  // namespace project::module

#endif  // PROJECT_MODULE_WIDGET_H
```

### Naming Conventions

```cpp
// NL.8 + NL.10: Consistent underscore_style
namespace my_project {

constexpr int max_buffer_size = 4096;  // NL.9: not ALL_CAPS (it's not a macro)

class tcp_connection {                 // underscore_style class
public:
    void send_message(std::string_view msg);
    bool is_connected() const;

private:
    std::string host_;                 // trailing underscore for members
    int port_;
};

}  // namespace my_project
```

### Anti-Patterns

- `using namespace std;` in a header at global scope (SF.7)
- Headers that depend on inclusion order (SF.10, SF.11)
- Hungarian notation like `strName`, `iCount` (NL.5)
- ALL_CAPS for anything other than macros (NL.9)

## C-Style Programming (CPL.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **CPL.1** | Prefer C++ to C |
| **CPL.2** | If you must use C, use the common subset of C and C++, and compile the C code as C++ |
| **CPL.3** | If you must use C for interfaces, use C++ in the calling code using such interfaces |

### Guidelines

* Avoid `void*` where a typed alternative exists
* Prefer `std::string` or `std::string_view` over C strings in owning or observing text
* Prefer `std::span` or range abstractions over pointer-plus-length where the project standard supports it
* Prefer typed enums, `constexpr`, and named constants over `#define` for constants and dispatch
* Prefer C++ casts over C-style casts
* Prefer RAII over manual cleanup
* Do not use C-style variadics in new C++ interfaces
* Do not use `memset`/`memcpy` to initialize or copy non-trivial types

### Anti-Patterns

- `void*` context pointers where a typed alternative is practical
- C-style casts (`(int)x`) instead of named C++ casts
- `#define` constants where `constexpr` or `enum class` applies
- C-style variadic functions in new interfaces
- `memset` used as object initialization
- `memcpy` used to copy non-trivial object state

## Performance (Per.*)

### Key Rules

| Rule | Summary |
|------|---------|
| **Per.1** | Don't optimize without reason |
| **Per.2** | Don't optimize prematurely |
| **Per.6** | Don't make claims about performance without measurements |
| **Per.7** | Design to enable optimization |
| **Per.10** | Rely on the static type system |
| **Per.11** | Move computation from run time to compile time |
| **Per.19** | Access memory predictably |

### Guidelines

```cpp
// Per.11: Compile-time computation where possible
constexpr auto lookup_table = [] {
    std::array<int, 256> table{};
    for (int i = 0; i < 256; ++i) {
        table[i] = i * i;
    }
    return table;
}();

// Per.19: Prefer contiguous data for cache-friendliness
std::vector<Point> points;           // GOOD: contiguous
std::vector<std::unique_ptr<Point>> indirect_points; // BAD: pointer chasing
```

### Anti-Patterns

- Optimizing without profiling data (Per.1, Per.6)
- Choosing "clever" low-level code over clear abstractions (Per.4, Per.5)
- Ignoring data layout and cache behavior (Per.19)

## Core Guidelines Profiles

The C++ Core Guidelines define machine-enforceable profiles for common
categories of errors. When available in the project's toolchain, these
profiles provide mechanical enforcement of safety properties.

### Type Safety Profile

Ensures objects are used only according to their type.

* Avoid casts that bypass the type system
* Do not use `union` to alias unrelated types
* Do not access a `union` member that was not the last one written
* Do not use `reinterpret_cast` unless necessary for low-level interop

### Bounds Safety Profile

Ensures array and buffer accesses are within bounds.

* Prefer `std::array` or `std::vector` over raw arrays
* Prefer `std::span` over pointer-plus-length in interfaces
* Use checked access where available
* Compiler and sanitizer bounds checks where available

### Lifetime Safety Profile

Ensures pointers and references do not outlive the objects they refer to.

* Prefer RAII for resource management
* Prefer scoped objects over heap-allocated objects
* Do not return references or pointers to local objects
* Use `std::unique_ptr` or `std::shared_ptr` for ownership transfer
* Static lifetime analysis where available

---

# Separate Guidelines From Project Policy

Clearly distinguish three categories of findings:

### Core Guideline

A recommendation grounded in a verified C++ Core Guidelines rule.

Example:

> **Core Guideline — R.1**
>
> The file descriptor is externally closed through `shutdown()`. Model the
> descriptor as an owning RAII resource or explain why object lifetime cannot
> represent the resource lifetime.

### C++ Design Heuristic

A synthesis rule used to detect C-with-classes or weak C++ abstraction.

Example:

> **C++ Design Heuristic — C-with-classes**
>
> `WorkerManager` is primarily a mutable state bag plus procedural lifecycle
> methods. The issue is not its naming or syntax; the abstraction does not
> express worker ownership or shutdown in its type/lifetime model.

### Project Contract

A repository-specific architectural or semantic requirement.

Example:

> **Project Contract**
>
> All backend contexts must support graceful shutdown via a `stop()` method.
> This is a project-level requirement, not a Core Guideline.

The skill must not fabricate a Core Guidelines rule ID for a project-specific
preference.

When reviewing code, findings should use these labels where useful.

---

# Respect The Project's C++ Standard

Before recommending a language or library facility, determine the project's
supported C++ standard.

Do not recommend C++23-only facilities in a C++20 project as though they are
available.

Common version-gated features:

| Feature | Minimum Standard |
|---------|-----------------|
| `std::optional`, `std::variant`, `std::any` | C++17 |
| `std::string_view` | C++17 |
| `if constexpr` | C++17 |
| Structured bindings | C++17 |
| Concepts, ranges, `std::span` | C++20 |
| `std::format` | C++20 |
| `std::expected` | C++23 |
| `std::flat_map`, `std::flat_set` | C++23 |

When a newer facility would materially improve the design:

* state the version requirement
* use an existing project abstraction if available
* otherwise use a compatible design

Do not perform a language-version migration unless explicitly requested.

---

# Tool Enforcement

Machine-checkable concerns should be enforced by tooling, not natural
language alone.

### Compiler warnings

* `-Wall -Wextra -Wpedantic` (minimum)
* `-Wconversion` for narrowing detection
* `-Wold-style-cast` for C-style cast detection
* `-Wnon-virtual-dtor` for missing virtual destructors

### clang-tidy checks

* `modernize-use-override` (C.128)
* `modernize-use-auto` (where appropriate)
* `modernize-use-nullptr` (ES.47)
* `modernize-use-emplace` (container efficiency)
* `cppcoreguidelines-owning-memory` (R.1, R.20)
* `cppcoreguidelines-pro-type-member-init` (ES.20)
* `cppcoreguidelines-init-variables` (ES.20)
* `cppcoreguidelines-avoid-c-arrays` (SL.con.1)
* `cppcoreguidelines-avoid-magic-numbers` (ES.45)
* `cppcoreguidelines-pro-type-union-access` (type safety)
* `cppcoreguidelines-pro-type-vararg` (F.55)

### Sanitizers

* AddressSanitizer (ASan) for memory errors
* ThreadSanitizer (TSan) for data races
* UndefinedBehaviorSanitizer (UBSan) for undefined behavior

### Static analysis

* Lifetime safety analysis where supported
* Bounds checking where supported

### Important caveat

Tools do not replace design review for:

* ownership model
* value semantics
* invariants
* abstraction boundaries
* shutdown semantics
* C-with-classes detection

---

# Completion Behavior

When this skill is active during implementation:

1. perform the C++ Design Synthesis Gate before introducing significant new
   types or interfaces
2. implement according to relevant Core Guidelines
3. respect project contracts and the supported C++ standard
4. run the Anti-C-With-Classes Review
5. review relevant type, bounds, and lifetime safety concerns
6. use available mechanical enforcement
7. report substantive unresolved deviations

When reviewing code, do not produce hundreds of low-value style findings.

Prioritize findings in this order:

1. lifetime and ownership
2. invalid states and invariants
3. resource safety
4. concurrency lifetime and synchronization
5. interface and type safety
6. bounds safety
7. error model
8. C-with-classes design
9. class and generic design
10. expressions and local coding issues
11. naming and layout

---

# Quick Reference Checklist

Before marking C++ work complete:

- [ ] No raw `new`/`delete` -- use smart pointers or RAII (R.11)
- [ ] Objects initialized at declaration (ES.20)
- [ ] Variables are `const`/`constexpr` by default (Con.1, ES.25)
- [ ] Member functions are `const` where possible (Con.2)
- [ ] `enum class` instead of plain `enum` (Enum.3)
- [ ] `nullptr` instead of `0`/`NULL` (ES.47)
- [ ] No narrowing conversions (ES.46)
- [ ] No C-style casts (ES.48)
- [ ] Single-argument constructors are `explicit` (C.46)
- [ ] Rule of Zero or Rule of Five applied (C.20, C.21)
- [ ] Base class destructors are public virtual or protected non-virtual (C.35)
- [ ] Templates are constrained with concepts (T.10)
- [ ] No `using namespace` in headers at global scope (SF.7)
- [ ] Headers have include guards and are self-contained (SF.8, SF.11)
- [ ] Locks use RAII (`scoped_lock`/`lock_guard`) (CP.20)
- [ ] Exceptions are custom types, thrown by value, caught by reference (E.14, E.15)
- [ ] `'\n'` instead of `std::endl` (SL.io.50)
- [ ] No magic numbers (ES.45)
- [ ] No `init()`/`shutdown()` protocols where RAII suffices (CPL.1)
- [ ] No output parameters where return values communicate intent (F.20)
- [ ] No C-style context pointers or manual lifecycle management (CPL.1)
- [ ] Interface does not leak C or OS abstractions to ordinary callers (CPL.3)
